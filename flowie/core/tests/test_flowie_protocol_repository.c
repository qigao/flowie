#include "flowie.h"
#include "flowie_orm_flow_internal.h"
#include "flowie_test_cnet.h"

#include <shared/orm/orm.h>
#include "tinytest.h"
#include <core/error.h>
#include <threads/thread_pool.h>
#include <threads/threads.h>

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static flowie_mqtt_span_t repository_span(const void *data, size_t size) {
  flowie_mqtt_span_t span = {(const uint8_t *)data, size};
  return span;
}

static flowie_protocol_repository_config_t repository_config(int create_schema) {
  static orm_config_t database;
  static orm_option_t option;
  flowie_protocol_repository_config_t config = FLOWIE_PROTOCOL_REPOSITORY_CONFIG_INIT;
  orm_config(&database);
  option.keyword = orm_view("filename");
  option.value = orm_view(":memory:");
  database.driver = orm_view("sqlite");
  database.options = &option;
  database.option_count = 1u;
  config.database = &database;
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
  return SALTS_OK;
}

static int visit_retained(void *context, const flowie_protocol_retained_row_t *row) {
  repository_visit_t *visit = (repository_visit_t *)context;
  static const uint8_t expected_payload[] = {'o', 'n'};
  ++visit->retained;
  check_equal(row->revision, 1u);
  check_equal(row->payload.size, sizeof(expected_payload));
  check_equal(memcmp(row->payload.data, expected_payload, sizeof(expected_payload)), 0);
  return SALTS_OK;
}

static int count_retained(void *context, const flowie_protocol_retained_row_t *row) {
  repository_visit_t *visit = (repository_visit_t *)context;
  (void)row;
  ++visit->retained;
  return SALTS_OK;
}

typedef struct repository_owner_visit_s {
  flowie_protocol_repository_t *repository;
  const void *owner_thread;
  const void *caller_thread;
  unsigned visits;
  int owner_consistent;
  int reentry_status;
  int status;
} repository_owner_visit_t;

static int visit_on_storage_owner(void *context, const flowie_protocol_retained_row_t *row) {
  repository_owner_visit_t *visit = (repository_owner_visit_t *)context;
  const void *thread = sync_thread_current_token();
  if (!visit->owner_thread) visit->owner_thread = thread;
  else if (visit->owner_thread != thread) visit->owner_consistent = 0;
  ++visit->visits;
  visit->reentry_status = flowie_protocol_repository_retained_delete(
      visit->repository, row->topic, row->revision);
  return SALTS_OK;
}

static void visit_from_another_caller(void *context) {
  repository_owner_visit_t *visit = (repository_owner_visit_t *)context;
  visit->caller_thread = sync_thread_current_token();
  visit->status = flowie_protocol_repository_retained_visit(
      visit->repository, visit_on_storage_owner, visit);
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
  status = flowie_orm_connect(&config, &connection, &error);
  if (status == ORM_STATUS_OK) status = orm_raw(connection, orm_view(sql), &query, &error);
  if (status == ORM_STATUS_OK) status = orm_query_execute(query, &result, &error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
  return status == ORM_STATUS_OK ? SALTS_OK : SALTS_EIO;
}

typedef struct repository_will_probe_s {
  atomic_int dispatched;
  atomic_int attempts;
  atomic_int ready;
} repository_will_probe_t;

static int repository_count_will(flowie_endpoint_core_t *endpoint, flowie_message_t *message,
                                  flowie_publish_result_t *result, void *ctx) {
  repository_will_probe_t *probe = ctx;
  (void)endpoint;
  (void)result;
  if ((message->flags & FLOWIE_MQTT_MESSAGE_BROKER_WILL) != 0u) {
    atomic_fetch_add(&probe->attempts, 1);
    if (!atomic_load(&probe->ready)) return SALTS_EBUSY;
    atomic_fetch_add(&probe->dispatched, 1);
  }
  return SALTS_OK;
}

static int repository_takeover_connect(flowie_test_cnet_client_t *client, int with_will) {
  static const uint8_t expiry[] = {0x11u, 0u, 0u, 0u, 60u};
  flowie_mqtt_connect_packet_t packet = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  uint8_t wire[128];
  size_t written;
  if (!client) return SALTS_EIO;
  packet.version = FLOWIE_MQTT_VERSION_5;
  packet.clean_start = 1u;
  packet.has_will = (uint8_t)with_will;
  packet.client_id = repository_span("durable-takeover", sizeof("durable-takeover") - 1u);
  packet.properties = repository_span(expiry, sizeof(expiry));
  if (with_will) packet.will_topic = repository_span("will", sizeof("will") - 1u);
  if (flowie_mqtt_connect_packet_encode(&packet, wire, sizeof(wire), &written) !=
      FLOWIE_MQTT_PARSE_OK)
    return SALTS_EPROTO;
  return flowie_test_cnet_send(client, wire, written);
}

static int repository_takeover_case(const char *path, flowie_protocol_repository_t *repository) {
  static const char reject_replacement[] =
      "CREATE TRIGGER reject_replacement BEFORE INSERT ON flowie_test_replaced_wills "
      "BEGIN SELECT RAISE(ABORT, 'replacement rejected'); END";
  static const uint8_t ping[] = {0xc0u, 0u};
  static const uint8_t pong[] = {0xd0u, 0u};
  flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
  flowie_execution_binding_t execution = FLOWIE_EXECUTION_BINDING_INIT;
  flowie_endpoint_persistence_binding_t persistence = FLOWIE_ENDPOINT_PERSISTENCE_BINDING_INIT;
  flowie_endpoint_bindings_t bindings = FLOWIE_ENDPOINT_BINDINGS_INIT;
  flowie_endpoint_core_t *endpoint = NULL;
  flowie_test_cnet_client_t *old = NULL;
  flowie_test_cnet_client_t *replacement = NULL;
  uint8_t reply[sizeof(pong)];
  repository_will_probe_t will = {0};
  const unsigned short port = flowie_test_cnet_port();
  int rc;

  if (port == 0u) return SALTS_EIO;
  config.host = "127.0.0.1";
  config.port = port;
  config.max_connections = 2u;
  config.max_sessions = 2u;
  config.max_subscriptions_per_session = 2u;
  config.max_inflight_per_session = 4u;
  config.max_packet_size = 128u;
  config.manage_sessions = 1;
  options.on_message = repository_count_will;
  options.message_ctx = &will;
  persistence.repository = repository;
  bindings.persistence = &persistence;

  rc = flowie_endpoint_core_create_ex("durable-takeover", &config, &options, &execution,
                                      &bindings, &endpoint);
  if (rc == SALTS_OK) rc = flowie_endpoint_core_start(endpoint);
  if (rc != SALTS_OK) goto done;
  old = flowie_test_cnet_connect(port);
  rc = repository_takeover_connect(old, 1);
  if (rc == SALTS_OK) rc = flowie_test_cnet_recv_mqtt5_connack(old, 0u, 4u, 128u);
  if (rc == SALTS_OK) rc = repository_execute_sqlite(path, reject_replacement);
  if (rc != SALTS_OK) goto done;

  replacement = flowie_test_cnet_connect(port);
  rc = repository_takeover_connect(replacement, 0);
  if (rc == SALTS_OK) rc = flowie_test_cnet_wait(replacement, &replacement->closed, 3000u);
  if (rc == SALTS_OK) rc = flowie_test_cnet_send(old, ping, sizeof(ping));
  if (rc == SALTS_OK) rc = flowie_test_cnet_recv_exact(old, reply, sizeof(reply));
  if (rc != SALTS_OK) goto done;
  if (memcmp(reply, pong, sizeof(pong)) != 0 || atomic_load(&will.attempts) != 0) {
    rc = SALTS_EPROTO;
    goto done;
  }

  rc = repository_execute_sqlite(path, "DROP TRIGGER reject_replacement");
  if (rc != SALTS_OK) goto done;
  flowie_test_cnet_close(replacement);
  replacement = flowie_test_cnet_connect(port);
  rc = repository_takeover_connect(replacement, 0);
  if (rc == SALTS_OK) rc = flowie_test_cnet_recv_mqtt5_connack(replacement, 0u, 4u, 128u);
  if (rc == SALTS_OK) rc = flowie_test_cnet_wait(old, &old->closed, 3000u);
  if (rc == SALTS_OK) {
    const uint64_t deadline = monotonic_ms() + 3000u;
    while (atomic_load(&will.attempts) == 0 && monotonic_ms() < deadline) sleep_ms(1u);
    if (atomic_load(&will.attempts) == 0 || atomic_load(&will.dispatched) != 0) rc = SALTS_EPROTO;
  }
  if (rc != SALTS_OK) goto done;
  rc = flowie_endpoint_core_stop(endpoint);
  if (rc != SALTS_OK) goto done;
  flowie_endpoint_core_destroy(endpoint);
  endpoint = NULL;
  config.port = flowie_test_cnet_port();
  if (config.port == 0) { rc = SALTS_EIO; goto done; }
  atomic_store(&will.ready, 1);
  rc = flowie_endpoint_core_create_ex("restored-takeover", &config, &options, &execution,
                                      &bindings, &endpoint);
  if (rc == SALTS_OK) rc = flowie_endpoint_core_start(endpoint);
  if (rc == SALTS_OK) {
    const uint64_t deadline = monotonic_ms() + 3000u;
    while (atomic_load(&will.dispatched) == 0 && monotonic_ms() < deadline) sleep_ms(1u);
    if (atomic_load(&will.dispatched) != 1) rc = SALTS_EPROTO;
  }

done:
  if (endpoint) {
    int stop_rc = flowie_endpoint_core_stop(endpoint);
    if (rc == SALTS_OK) rc = stop_rc;
  }
  flowie_test_cnet_close(replacement);
  flowie_test_cnet_close(old);
  flowie_endpoint_core_destroy(endpoint);
  return rc;
}

spec("flowie typed protocol repository") {
  static flowie_protocol_repository_t *repository;
  static char *path;
  static threadpool_t *caller;

  before_each() {
    repository = NULL;
    path = NULL;
    caller = NULL;
  }

  after_each() {
    if (caller) threadpool_destroy(caller);
    flowie_protocol_repository_close(repository);
    if (path) {
      const int remove_status = tt_remove_file(path);
      free(path);
      path = NULL;
      check_equal(remove_status, 0);
    }
  }

  it("rolls back a failed replacement and restores a committed predecessor Will") {
    flowie_protocol_repository_config_t config = repository_config(1);
    orm_config_t database = *config.database;
    orm_option_t filename = *database.options;
    path = tt_make_temp_file("flowie-takeover", ".sqlite3");
    check_not_null(path);
    filename.value = orm_view(path);
    database.options = &filename;
    config.database = &database;
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_OK);
    check_equal(repository_takeover_case(path, repository), SALTS_OK);
  }

  it("keeps storage on one owner across caller threads and rejects callback reentry") {
    const threadpool_config_t worker = {1, 1u};
    flowie_protocol_repository_config_t config = repository_config(1);
    flowie_protocol_retained_row_t row = FLOWIE_PROTOCOL_RETAINED_ROW_INIT;
    repository_owner_visit_t visit = {0};
    const void *main_thread = sync_thread_current_token();
    static const char topic[] = "owner/thread/test";
    static const char payload[] = "retained";
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_OK);
    row.topic = repository_span(topic, sizeof(topic) - 1u);
    row.payload = repository_span(payload, sizeof(payload) - 1u);
    row.revision = 1u;
    row.publisher_session_id = 1u;
    row.mqtt_version = FLOWIE_MQTT_VERSION_5;
    check_equal(flowie_protocol_repository_retained_save(repository, &row), SALTS_OK);
    visit.repository = repository;
    visit.owner_consistent = 1;
    check_equal(flowie_protocol_repository_retained_visit(
        repository, visit_on_storage_owner, &visit), SALTS_OK);
    check_equal(visit.reentry_status, SALTS_EBUSY);
    caller = threadpool_create_with_config(&worker);
    check_not_null(caller);
    check_equal(threadpool_submit(caller, visit_from_another_caller, &visit), SALTS_OK);
    check_equal(threadpool_wait_status(caller), SALTS_OK);
    check_equal(visit.status, SALTS_OK);
    check_equal(visit.visits, 2u);
    check_equal(visit.owner_consistent, 1);
    check_not_equal(visit.owner_thread, main_thread);
    check_not_equal(visit.owner_thread, visit.caller_thread);
    check_equal(visit.reentry_status, SALTS_EBUSY);
    check_equal(flowie_protocol_repository_retained_delete(repository, row.topic, 1u), SALTS_OK);
  }

  it("rejects mismatched TurboDB configuration ABIs") {
    flowie_protocol_repository_config_t config = repository_config(1);
    orm_config_t database = *config.database;
    config.database = &database;

    database.struct_size = sizeof(database) - 1u;
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_EINVAL);
    check_null(repository);
    database.struct_size = sizeof(database) + 1u;
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_EINVAL);
    check_null(repository);
    database.struct_size = sizeof(database);
    ++database.abi_version;
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_EINVAL);
    check_null(repository);
  }

  it("rejects a database without the current typed schema") {
    flowie_protocol_repository_config_t config = repository_config(0);
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_EIO);
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
    flowie_protocol_subscription_row_t subscription = {
        repository_span(filter, strlen(filter)), 1u, 0u, 1u, 0u, 17u};
    flowie_protocol_inflight_row_t inflight = {22u, 2u};
    flowie_protocol_delivery_row_t delivery = {23u, 1u, 1u, 900u,
                                               repository_span(packet, sizeof(packet))};
    flowie_protocol_session_row_t session = FLOWIE_PROTOCOL_SESSION_ROW_INIT;
    flowie_protocol_retained_row_t retained = FLOWIE_PROTOCOL_RETAINED_ROW_INIT;
    repository_visit_t visit = {0u, 0u};

    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_OK);
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
    check_equal(flowie_protocol_repository_session_save(repository, &session), SALTS_OK);
    check_equal(flowie_protocol_repository_session_save(repository, &session), SALTS_EBUSY);
    check_equal(flowie_protocol_repository_session_visit(repository, visit_session, &visit),
                SALTS_OK);
    check_equal(visit.sessions, 1u);

    retained.topic = repository_span(filter, strlen(filter));
    retained.revision = 1u;
    retained.publisher_session_id = session.session_id;
    retained.mqtt_version = FLOWIE_MQTT_VERSION_5;
    retained.qos = 1u;
    retained.properties = repository_span(properties, sizeof(properties));
    retained.payload = repository_span(payload, sizeof(payload));
    check_equal(flowie_protocol_repository_retained_save(repository, &retained), SALTS_OK);
    check_equal(flowie_protocol_repository_retained_save(repository, &retained), SALTS_EBUSY);
    check_equal(flowie_protocol_repository_retained_visit(repository, visit_retained, &visit),
                SALTS_OK);
    check_equal(visit.retained, 1u);
    check_equal(flowie_protocol_repository_retained_delete(repository, retained.topic, 1u),
                SALTS_OK);
    check_equal(flowie_protocol_repository_session_delete(repository, session.client_id, 1u),
                SALTS_OK);
  }

  it("reports retained rows beyond the configured limit as capacity exhaustion") {
    orm_config_t database;
    orm_option_t option;
    static const char first_row_sql[] =
        "INSERT INTO flowie_test_retained VALUES('topic-1',1,1,0,5,1,X'',X'01')";
    static const char second_row_sql[] =
        "INSERT INTO flowie_test_retained VALUES('topic-2',1,1,0,5,1,X'',X'02')";
    path = tt_make_temp_file("flowie-protocol-limit", ".sqlite3");
    flowie_protocol_repository_config_t config = repository_config(1);
    repository_visit_t visit = {0u, 0u};

    check_not_null(path);
    database = *config.database;
    option.keyword = orm_view("filename");
    option.value = orm_view(path);
    database.options = &option;
    config.database = &database;
    config.limits.max_sessions = 1u;
    config.limits.max_retained_messages = 1u;
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_OK);
    check_equal(repository_execute_sqlite(path, first_row_sql), SALTS_OK);
    check_equal(flowie_protocol_repository_retained_visit(repository, count_retained, &visit),
                SALTS_OK);
    check_equal(visit.retained, 1u);
    check_equal(repository_execute_sqlite(path, second_row_sql), SALTS_OK);
    visit.retained = 0u;
    check_equal(flowie_protocol_repository_retained_visit(repository, count_retained, &visit),
                SALTS_ENOSPC);
    check_equal(visit.retained, 1u);

  }

  it("rejects unrepresentable result row budgets") {
    flowie_protocol_repository_config_t config = repository_config(1);

    config.limits.max_sessions = SIZE_MAX;
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_EINVAL);
    check_null(repository);
    config = repository_config(1);
    config.limits.max_retained_messages = SIZE_MAX;
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_EINVAL);
    check_null(repository);
    config = repository_config(1);
    config.limits.max_subscriptions_per_session = SIZE_MAX;
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_EINVAL);
    check_null(repository);
    config = repository_config(1);
    config.limits.max_inflight_per_session = SIZE_MAX;
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_EINVAL);
    check_null(repository);
  }
}
