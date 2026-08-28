#include "flowie_protocol_repository.h"

#include "orm.h"
#include "tinytest.h"
#include "turbo_error.h"

#include <stdint.h>
#include <string.h>

static flowie_mqtt_span_t repository_span(const void *data, size_t size) {
  flowie_mqtt_span_t span = {(const uint8_t *)data, size};
  return span;
}

static flowie_protocol_repository_config_t repository_config(int create_schema) {
  static const flowie_protocol_repository_option_t options[] = {{"filename", ":memory:"}};
  flowie_protocol_repository_config_t config = FLOWIE_PROTOCOL_REPOSITORY_CONFIG_INIT;
  config.driver = "sqlite";
  config.options = options;
  config.option_count = 1u;
  config.namespace_name = "flowie_test";
  config.create_schema = create_schema;
  config.limits.max_sessions = 8u;
  config.limits.max_subscriptions_per_session = 8u;
  config.limits.max_inflight_per_session = 8u;
  config.limits.max_retained_messages = 8u;
  config.limits.max_client_id_size = 128u;
  config.limits.max_topic_size = 256u;
  config.limits.max_packet_size = 1024u;
  return config;
}

typedef struct repository_visit_s {
  unsigned sessions;
  unsigned retained;
} repository_visit_t;

static int visit_session(void *context, const flowie_protocol_session_row_t *row) {
  repository_visit_t *visit = (repository_visit_t *)context;
  static const uint8_t expected_packet[] = {0x32u, 0x03u, 'x'};
  ++visit->sessions;
  check_equal(row->revision, 1u);
  check_equal(row->session_id, 7u);
  check_equal(row->subscription_count, 1u);
  check_equal(row->inflight_count, 1u);
  check_equal(row->delivery_count, 1u);
  check_equal(row->deliveries[0].packet.size, sizeof(expected_packet));
  check_equal(memcmp(row->deliveries[0].packet.data, expected_packet, sizeof(expected_packet)), 0);
  check_equal(row->will.present, 1);
  check_equal(row->principal.role_count, 1u);
  check_equal(row->principal.roles[0], "operator");
  return TURBO_OK;
}

static int visit_retained(void *context, const flowie_protocol_retained_row_t *row) {
  repository_visit_t *visit = (repository_visit_t *)context;
  static const uint8_t expected_payload[] = {'o', 'n'};
  ++visit->retained;
  check_equal(row->revision, 1u);
  check_equal(row->payload.size, sizeof(expected_payload));
  check_equal(memcmp(row->payload.data, expected_payload, sizeof(expected_payload)), 0);
  return TURBO_OK;
}

static int count_retained(void *context, const flowie_protocol_retained_row_t *row) {
  repository_visit_t *visit = (repository_visit_t *)context;
  (void)row;
  ++visit->retained;
  return TURBO_OK;
}

static int repository_execute_sqlite(const char *path, const char *sql) {
  orm_error_t error;
  orm_option_t filename;
  orm_config_t config;
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status;
  orm_error_init(&error);
  orm_config(&config);
  filename.keyword = orm_view("filename");
  filename.value = orm_view(path);
  config.driver = orm_view("sqlite");
  config.options = &filename;
  config.option_count = 1u;
  status = orm_connect(&config, &connection, &error);
  if (status == ORM_STATUS_OK) status = orm_raw(connection, orm_view(sql), &query, &error);
  if (status == ORM_STATUS_OK) status = orm_query_execute(query, &result, &error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
  return status == ORM_STATUS_OK ? TURBO_OK : TURBO_EIO;
}

spec("flowie typed protocol repository") {
  it("rejects a database without the current typed schema") {
    flowie_protocol_repository_config_t config = repository_config(0);
    flowie_protocol_repository_t *repository = NULL;
    check_equal(flowie_protocol_repository_open(&config, &repository), TURBO_EIO);
    check_null(repository);
  }

  it("round trips typed session state and retained payloads with CAS") {
    static const char client_id[] = "client-a";
    static const char filter[] = "sensors/+";
    static const char will_topic[] = "status/client-a";
    static const uint8_t packet[] = {0x32u, 0x03u, 'x'};
    static const uint8_t properties[] = {0x01u, 0x02u};
    static const uint8_t payload[] = {'o', 'n'};
    flowie_protocol_repository_config_t config = repository_config(1);
    flowie_protocol_repository_t *repository = NULL;
    flowie_protocol_subscription_row_t subscription = {
        repository_span(filter, strlen(filter)), 1u, 0u, 1u, 0u, 17u};
    flowie_protocol_inflight_row_t inflight = {22u, 2u};
    flowie_protocol_delivery_row_t delivery = {23u, 1u, 1u, 900u,
                                               repository_span(packet, sizeof(packet))};
    flowie_protocol_session_row_t session = FLOWIE_PROTOCOL_SESSION_ROW_INIT;
    flowie_protocol_retained_row_t retained = FLOWIE_PROTOCOL_RETAINED_ROW_INIT;
    repository_visit_t visit = {0u, 0u};

    check_equal(flowie_protocol_repository_open(&config, &repository), TURBO_OK);
    session.client_id = repository_span(client_id, strlen(client_id));
    session.revision = 1u;
    session.session_id = 7u;
    session.session_generation = 3u;
    session.mqtt_version = FLOWIE_MQTT_VERSION_5;
    session.keep_alive = 30u;
    session.session_expiry_interval = 3600u;
    session.next_delivery_packet_id = 24u;
    session.has_principal = 1;
    strcpy(session.principal.principal_id, "alice");
    strcpy(session.principal.principal_type, "user");
    strcpy(session.principal.auth_method, "password");
    session.principal.scope = FLOWIE_SECURITY_SCOPE_SELF;
    session.principal.role_count = 1u;
    strcpy(session.principal.roles[0], "operator");
    session.subscriptions = &subscription;
    session.subscription_count = 1u;
    session.inflight = &inflight;
    session.inflight_count = 1u;
    session.deliveries = &delivery;
    session.delivery_count = 1u;
    session.will.present = 1;
    session.will.pending = 1;
    session.will.qos = 1u;
    session.will.topic = repository_span(will_topic, strlen(will_topic));
    session.will.properties = repository_span(properties, sizeof(properties));
    session.will.payload = repository_span(payload, sizeof(payload));
    check_equal(flowie_protocol_repository_session_save(repository, &session), TURBO_OK);
    check_equal(flowie_protocol_repository_session_save(repository, &session), TURBO_EBUSY);
    check_equal(flowie_protocol_repository_session_visit(repository, visit_session, &visit),
                TURBO_OK);
    check_equal(visit.sessions, 1u);

    retained.topic = repository_span(filter, strlen(filter));
    retained.revision = 1u;
    retained.publisher_session_id = session.session_id;
    retained.mqtt_version = FLOWIE_MQTT_VERSION_5;
    retained.qos = 1u;
    retained.properties = repository_span(properties, sizeof(properties));
    retained.payload = repository_span(payload, sizeof(payload));
    check_equal(flowie_protocol_repository_retained_save(repository, &retained), TURBO_OK);
    check_equal(flowie_protocol_repository_retained_save(repository, &retained), TURBO_EBUSY);
    check_equal(flowie_protocol_repository_retained_visit(repository, visit_retained, &visit),
                TURBO_OK);
    check_equal(visit.retained, 1u);
    check_equal(flowie_protocol_repository_retained_delete(repository, retained.topic, 1u),
                TURBO_OK);
    check_equal(flowie_protocol_repository_session_delete(repository, session.client_id, 1u),
                TURBO_OK);
    flowie_protocol_repository_close(repository);
  }

  it("reports retained rows beyond the configured limit as capacity exhaustion") {
    static flowie_protocol_repository_option_t options[] = {{"filename", NULL}};
    static const char first_row_sql[] =
        "INSERT INTO flowie_test_retained VALUES('topic-1',1,1,0,5,1,X'',X'01')";
    static const char second_row_sql[] =
        "INSERT INTO flowie_test_retained VALUES('topic-2',1,1,0,5,1,X'',X'02')";
    char *path = tt_make_temp_file("flowie-protocol-limit", ".sqlite3");
    flowie_protocol_repository_config_t config = repository_config(1);
    flowie_protocol_repository_t *repository = NULL;
    repository_visit_t visit = {0u, 0u};

    check_not_null(path);
    options[0].value = path;
    config.options = options;
    config.limits.max_sessions = 1u;
    config.limits.max_retained_messages = 1u;
    check_equal(flowie_protocol_repository_open(&config, &repository), TURBO_OK);
    check_equal(repository_execute_sqlite(path, first_row_sql), TURBO_OK);
    check_equal(flowie_protocol_repository_retained_visit(repository, count_retained, &visit),
                TURBO_OK);
    check_equal(visit.retained, 1u);
    check_equal(repository_execute_sqlite(path, second_row_sql), TURBO_OK);
    visit.retained = 0u;
    check_equal(flowie_protocol_repository_retained_visit(repository, count_retained, &visit),
                TURBO_ENOSPC);
    check_equal(visit.retained, 1u);

    flowie_protocol_repository_close(repository);
    check_equal(tt_remove_file(path), 0);
  }

  it("rejects unrepresentable result row budgets") {
    flowie_protocol_repository_config_t config = repository_config(1);
    flowie_protocol_repository_t *repository = NULL;

    config.limits.max_sessions = SIZE_MAX;
    check_equal(flowie_protocol_repository_open(&config, &repository), TURBO_EINVAL);
    check_null(repository);
    config = repository_config(1);
    config.limits.max_retained_messages = SIZE_MAX;
    check_equal(flowie_protocol_repository_open(&config, &repository), TURBO_EINVAL);
    check_null(repository);
    config = repository_config(1);
    config.limits.max_subscriptions_per_session = SIZE_MAX;
    check_equal(flowie_protocol_repository_open(&config, &repository), TURBO_EINVAL);
    check_null(repository);
    config = repository_config(1);
    config.limits.max_inflight_per_session = SIZE_MAX;
    check_equal(flowie_protocol_repository_open(&config, &repository), TURBO_EINVAL);
    check_null(repository);
  }
}
