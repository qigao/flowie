#include "flowie_protocol_repository.h"

#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include "orm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static flowie_mqtt_span_t pgsql_span(const void *data, size_t size) {
  flowie_mqtt_span_t span = {(const uint8_t *)data, size};
  return span;
}

static void pgsql_drop_tables(const char *conninfo, const char *prefix) {
  static const char sql_format[] = "DROP TABLE IF EXISTS %s_wills,%s_deliveries,%s_inflight,"
                                   "%s_subscriptions,%s_principal_groups,%s_principal_roles,"
                                   "%s_retained,%s_sessions,%s_meta";
  char sql[1024];
  orm_config_t config;
  orm_option_t option;
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_error_t error;
  const int written = snprintf(sql, sizeof(sql), sql_format, prefix, prefix, prefix, prefix, prefix,
                               prefix, prefix, prefix, prefix);
  check_true(written > 0);
  check_true((size_t)written < sizeof(sql));
  orm_config(&config);
  orm_error_init(&error);
  option.keyword = orm_view("conninfo");
  option.value = orm_view(conninfo);
  config.driver = orm_view("postgresql");
  config.options = &option;
  config.option_count = 1u;
  check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);
  check_equal(orm_raw(connection, orm_view(sql), &query, &error), ORM_STATUS_OK);
  check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
}

typedef struct pgsql_visit_s {
  size_t sessions;
} pgsql_visit_t;

static int pgsql_visit_session(void *ctx, const flowie_protocol_session_row_t *row) {
  static const uint8_t expected_packet[] = {0x32u, 0x03u, 'p'};
  pgsql_visit_t *visit = (pgsql_visit_t *)ctx;
  ++visit->sessions;
  check_equal(row->client_id.size, strlen("pgsql-client"));
  check_equal(memcmp(row->client_id.data, "pgsql-client", row->client_id.size), 0);
  check_equal(row->subscription_count, 1u);
  check_equal(row->subscriptions[0].filter.size, strlen("pgsql/topic/+"));
  check_equal(row->inflight_count, 1u);
  check_equal(row->inflight[0].packet_id, 22u);
  check_equal(row->delivery_count, 1u);
  check_equal(row->deliveries[0].packet.size, sizeof(expected_packet));
  check_equal(memcmp(row->deliveries[0].packet.data, expected_packet, sizeof(expected_packet)), 0);
  check_equal(row->principal.role_count, 1u);
  check_equal(row->principal.roles[0], "operator");
  check_equal(row->principal.group_count, 1u);
  check_equal(row->principal.groups[0], "devices");
  return TURBO_OK;
}

spec("flowie typed PostgreSQL protocol repository") {
  it("closes the session header Source before loading child rows") {
    static const char client_id[] = "pgsql-client";
    static const char filter[] = "pgsql/topic/+";
    static const uint8_t packet[] = {0x32u, 0x03u, 'p'};
    const char *conninfo = getenv("TURBO_FLOW_PGSQL_TEST_CONNINFO");
    char prefix[96];
    flowie_protocol_repository_option_t option = {"conninfo", NULL};
    flowie_protocol_repository_config_t config = FLOWIE_PROTOCOL_REPOSITORY_CONFIG_INIT;
    flowie_protocol_repository_t *repository = NULL;
    flowie_protocol_subscription_row_t subscription = {
        pgsql_span(filter, strlen(filter)), 1u, 0u, 1u, 0u, 17u};
    flowie_protocol_inflight_row_t inflight = {22u, 2u};
    flowie_protocol_delivery_row_t delivery = {23u, 1u, 1u, 900u,
                                               pgsql_span(packet, sizeof(packet))};
    flowie_protocol_session_row_t session = FLOWIE_PROTOCOL_SESSION_ROW_INIT;
    pgsql_visit_t visit = {0u};

    check_not_null(conninfo);
    check_true(conninfo[0] != '\0');
    option.value = conninfo;
    (void)snprintf(prefix, sizeof(prefix), "flowie_protocol_pg_%llu",
                   (unsigned long long)turbo_hrtime());
    config.driver = "postgresql";
    config.options = &option;
    config.option_count = 1u;
    config.namespace_name = prefix;
    config.create_schema = 1;
    config.limits.max_sessions = 8u;
    config.limits.max_subscriptions_per_session = 8u;
    config.limits.max_inflight_per_session = 8u;
    config.limits.max_retained_messages = 8u;
    config.limits.max_client_id_size = 128u;
    config.limits.max_topic_size = 256u;
    config.limits.max_packet_size = 1024u;
    check_equal(flowie_protocol_repository_open(&config, &repository), TURBO_OK);

    session.client_id = pgsql_span(client_id, strlen(client_id));
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
    check_equal(flowie_protocol_repository_session_save(repository, &session), TURBO_OK);
    check_equal(flowie_protocol_repository_session_visit(repository, pgsql_visit_session, &visit),
                TURBO_OK);
    check_equal(visit.sessions, 1u);

    flowie_protocol_repository_close(repository);
    pgsql_drop_tables(conninfo, prefix);
  }
}
