#include "flowie_protocol_repository.h"
#include "flowie_orm_flow_internal.h"

#include "orm.h"
#include "tinytest.h"
#include "salts_error.h"
#include "salts_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static flowie_mqtt_span_t live_span(const void *data, size_t size) {
  const flowie_mqtt_span_t span = {(const uint8_t *)data, size};
  return span;
}

static int live_drop_tables(const orm_config_t *database, const char *prefix) {
  static const char sql_format[] = "DROP TABLE IF EXISTS %s_wills,%s_deliveries,%s_inflight,"
                                   "%s_subscriptions,%s_principal_groups,%s_principal_roles,"
                                   "%s_retained,%s_sessions,%s_meta";
  char sql[1024];
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_error_t error;
  orm_status_t status;
  const int written = snprintf(sql, sizeof(sql), sql_format, prefix, prefix, prefix, prefix, prefix,
                               prefix, prefix, prefix, prefix);
  if (written <= 0 || (size_t)written >= sizeof(sql)) return SALTS_ERANGE;
  orm_error_init(&error);
  status = flowie_orm_connect(database, &connection, &error);
  if (status == ORM_STATUS_OK) status = orm_raw(connection, orm_view(sql), &query, &error);
  if (status == ORM_STATUS_OK) status = orm_query_execute(query, &result, &error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
  return status == ORM_STATUS_OK ? SALTS_OK : SALTS_EIO;
}

typedef struct live_visit_s {
  size_t sessions;
} live_visit_t;

static int live_visit_session(void *context, const flowie_protocol_session_row_t *row) {
  static const uint8_t expected_packet[] = {0x32u, 0x03u, 'p'};
  live_visit_t *visit = (live_visit_t *)context;
  ++visit->sessions;
  check_equal(row->client_id.size, strlen("turbodb-client"));
  check_equal(memcmp(row->client_id.data, "turbodb-client", row->client_id.size), 0);
  check_equal(row->subscription_count, 1u);
  check_equal(row->subscriptions[0].filter.size, strlen("turbodb/topic/+"));
  check_equal(row->inflight_count, 1u);
  check_equal(row->inflight[0].packet_id, 22u);
  check_equal(row->delivery_count, 1u);
  check_equal(row->deliveries[0].packet.size, sizeof(expected_packet));
  check_equal(memcmp(row->deliveries[0].packet.data, expected_packet, sizeof(expected_packet)), 0);
  check_equal(row->principal.role_count, 1u);
  check_equal(row->principal.roles[0], "operator");
  check_equal(row->principal.group_count, 1u);
  check_equal(row->principal.groups[0], "devices");
  return SALTS_OK;
}

spec("flowie TurboDB live protocol repository") {
  it("persists the repository contract through the configured TurboDB driver") {
    static const char client_id[] = "turbodb-client";
    static const char filter[] = "turbodb/topic/+";
    static const uint8_t packet[] = {0x32u, 0x03u, 'p'};
    const char *conninfo = getenv("FLOWIE_TURBODB_TEST_CONNINFO");
    char prefix[96];
    orm_config_t database;
    orm_option_t option;
    flowie_protocol_repository_config_t config = FLOWIE_PROTOCOL_REPOSITORY_CONFIG_INIT;
    flowie_protocol_repository_t *repository = NULL;
    flowie_protocol_subscription_row_t subscription = {
        live_span(filter, strlen(filter)), 1u, 0u, 1u, 0u, 17u};
    flowie_protocol_inflight_row_t inflight = {22u, 2u};
    flowie_protocol_delivery_row_t delivery = {23u, 1u, 1u, 900u,
                                               live_span(packet, sizeof(packet))};
    flowie_protocol_session_row_t session = FLOWIE_PROTOCOL_SESSION_ROW_INIT;
    live_visit_t visit = {0u};

    if (!conninfo || !conninfo[0]) {
      check_true(0 && "FLOWIE_TURBODB_TEST_CONNINFO is required");
      return;
    }
    orm_config(&database);
    option.keyword = orm_view("conninfo");
    option.value = orm_view(conninfo);
    database.driver = orm_view("postgresql");
    database.options = &option;
    database.option_count = 1u;
    (void)snprintf(prefix, sizeof(prefix), "flowie_protocol_turbodb_%llu",
                   (unsigned long long)salts_hrtime());
    config.database = &database;
    config.namespace_name = prefix;
    config.create_schema = 1;
    config.limits.max_sessions = 8u;
    config.limits.max_subscriptions_per_session = 8u;
    config.limits.max_inflight_per_session = 8u;
    config.limits.max_retained_messages = 8u;
    config.limits.max_client_id_size = 128u;
    config.limits.max_topic_size = 256u;
    config.limits.max_packet_size = 1024u;
    check_equal(flowie_protocol_repository_open(&config, &repository), SALTS_OK);
    if (!repository) return;

    session.client_id = live_span(client_id, strlen(client_id));
    session.revision = 1u;
    session.session_id = 7u;
    session.session_generation = 3u;
    session.mqtt_version = FLOWIE_MQTT_VERSION_5;
    session.keep_alive = 30u;
    session.next_delivery_packet_id = 24u;
    session.has_principal = 1;
    strcpy(session.principal.principal_id, "alice");
    strcpy(session.principal.principal_type, "user");
    strcpy(session.principal.domain_id, "root-a");
    strcpy(session.principal.auth_method, "password");
    session.principal.scope = FLOWIE_SECURITY_SCOPE_SELF;
    session.principal.role_count = 1u;
    strcpy(session.principal.roles[0], "operator");
    session.principal.group_count = 1u;
    strcpy(session.principal.groups[0], "devices");
    session.subscriptions = &subscription;
    session.subscription_count = 1u;
    session.inflight = &inflight;
    session.inflight_count = 1u;
    session.deliveries = &delivery;
    session.delivery_count = 1u;
    check_equal(flowie_protocol_repository_session_save(repository, &session), SALTS_OK);
    check_equal(flowie_protocol_repository_session_visit(repository, live_visit_session, &visit),
                SALTS_OK);
    check_equal(visit.sessions, 1u);

    flowie_protocol_repository_close(repository);
    check_equal(live_drop_tables(&database, prefix), SALTS_OK);
  }
}
