#include "flowie_control_management_rpc_internal.h"
#include "flowie_control_test_turbodb.h"

#include "platform.h"
#include "CoroNet.h"
#include "flowie_control_credential_internal.h"
#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_parser.h"
#include "turbo_thread.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum management_rpc_policy_operation_e {
  MANAGEMENT_RPC_POLICY_VALIDATE = 1,
  MANAGEMENT_RPC_POLICY_PUBLISH = 2,
  MANAGEMENT_RPC_POLICY_DRY_RUN = 3
} management_rpc_policy_operation_t;

typedef struct management_rpc_policy_gate_s {
  management_rpc_policy_operation_t operation;
  atomic_int entered;
  atomic_int completed;
  atomic_int release;
  atomic_int status_completed;
} management_rpc_policy_gate_t;

static _Atomic(management_rpc_policy_gate_t *) management_rpc_active_policy_gate;

typedef struct management_rpc_fixture_s {
  flowie_control_management_caller_t caller;
  uint64_t now;
  int resolver_rc;
  int external_https_enabled;
  size_t external_https_stats_calls;
  flowie_control_external_https_authenticator_stats_t external_https_stats;
  management_rpc_policy_gate_t *policy_gate;
  flowie_control_repository_t repository;
  flowie_control_repository_policy_ops_t policy_ops;
} management_rpc_fixture_t;

static int management_rpc_policy_gate_wait(management_rpc_policy_operation_t operation) {
  management_rpc_policy_gate_t *gate =
      atomic_load_explicit(&management_rpc_active_policy_gate, memory_order_acquire);
  if (!gate || gate->operation != operation) return TURBO_EINVAL;
  atomic_store_explicit(&gate->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&gate->release, memory_order_acquire))
    turbo_sleep_ms(1u);
  atomic_store_explicit(&gate->completed, 1, memory_order_release);
  return TURBO_OK;
}

static int management_rpc_policy_validate(void *ctx, const char *domain_id,
                                          flowie_control_policy_validation_t *out) {
  int rc;
  (void)ctx;
  if (!domain_id || !out || out->size < sizeof(*out)) return TURBO_EINVAL;
  rc = management_rpc_policy_gate_wait(MANAGEMENT_RPC_POLICY_VALIDATE);
  if (rc != TURBO_OK) return rc;
  out->store_revision = 2u;
  out->rule_count = 3u;
  out->deny_rule_count = 1u;
  return TURBO_OK;
}

static int management_rpc_policy_publish(void *ctx,
                                         const flowie_control_policy_publish_command_t *command,
                                         flowie_control_policy_publish_result_t *result) {
  int rc;
  (void)ctx;
  if (!command || command->size < sizeof(*command) || !result || result->size < sizeof(*result))
    return TURBO_EINVAL;
  rc = management_rpc_policy_gate_wait(MANAGEMENT_RPC_POLICY_PUBLISH);
  if (rc != TURBO_OK) return rc;
  result->revision = 3u;
  result->policy_version = 2u;
  result->replayed = 0;
  return TURBO_OK;
}

static int management_rpc_policy_dry_run(void *ctx, const char *domain_id,
                                         const flowie_control_policy_dry_run_change_t *changes,
                                         size_t change_count,
                                         flowie_control_policy_dry_run_result_t *result) {
  int rc;
  (void)ctx;
  if (!domain_id || !changes || change_count == 0u || !result || result->size < sizeof(*result))
    return TURBO_EINVAL;
  rc = management_rpc_policy_gate_wait(MANAGEMENT_RPC_POLICY_DRY_RUN);
  if (rc != TURBO_OK) return rc;
  result->valid = 1;
  result->store_revision = 2u;
  result->rule_count = 2u;
  result->deny_rule_count = 0u;
  result->diagnostic_count = 0u;
  return TURBO_OK;
}

static int management_rpc_resolve(void *ctx, const Req *request,
                                  flowie_control_management_caller_t *caller_out) {
  management_rpc_fixture_t *fixture = (management_rpc_fixture_t *)ctx;
  (void)request;
  if (fixture->resolver_rc != TURBO_OK) return fixture->resolver_rc;
  *caller_out = fixture->caller;
  return TURBO_OK;
}

static uint64_t management_rpc_clock(void *ctx) { return ((management_rpc_fixture_t *)ctx)->now; }

static int management_rpc_external_https_stats(
    void *ctx, flowie_control_external_https_authenticator_stats_t *stats_out) {
  management_rpc_fixture_t *fixture = (management_rpc_fixture_t *)ctx;
  if (!fixture || !stats_out || stats_out->size < sizeof(*stats_out)) return TURBO_EINVAL;
  ++fixture->external_https_stats_calls;
  if (!fixture->external_https_enabled) return TURBO_ENOENT;
  *stats_out = fixture->external_https_stats;
  return TURBO_OK;
}

static flowie_control_management_rpc_server_t *
management_rpc_open(char **path_out, flowie_control_store_t **store_out,
                    flowie_control_management_service_t **service_out, rpc_context_t **rpc_out,
                    iris_app_t **app_out, management_rpc_fixture_t *fixture) {
  flowie_control_store_config_t store_config = FLOWIE_CONTROL_STORE_CONFIG_INIT;
  flowie_control_test_turbodb_t test_database;
  flowie_control_domain_create_command_t root = FLOWIE_CONTROL_DOMAIN_CREATE_COMMAND_INIT;
  flowie_control_command_result_t root_result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  flowie_control_management_service_config_t service_config =
      FLOWIE_CONTROL_MANAGEMENT_SERVICE_CONFIG_INIT;
  flowie_control_management_rpc_server_config_t server_config =
      FLOWIE_CONTROL_MANAGEMENT_RPC_SERVER_CONFIG_INIT;
  rpc_config_t rpc_config = RPC_DEFAULT_CONFIG();
  flowie_control_management_rpc_server_t *server = NULL;

  *path_out = tt_make_temp_file("flowie-management-rpc", ".sqlite3");
  check_not_null(*path_out);
  check_equal(flowie_control_test_turbodb_init(&test_database, *path_out), 0);
  store_config.database = &test_database.config;
  check_equal(flowie_control_store_open(&store_config, store_out), TURBO_OK);
  root.domain_id = "root-a";
  root.actor = "bootstrap";
  root.request_id = "request-root";
  root.occurred_at = 1000u;
  check_equal(flowie_control_store_domain_create(*store_out, &root, &root_result), TURBO_OK);
  service_config.repository = flowie_control_store_repository(*store_out);
  if (fixture->policy_gate) {
    fixture->repository = *service_config.repository;
    fixture->policy_ops = *fixture->repository.policy;
    fixture->policy_ops.validate = management_rpc_policy_validate;
    fixture->policy_ops.dry_run = management_rpc_policy_dry_run;
    fixture->policy_ops.publish = management_rpc_policy_publish;
    fixture->repository.policy = &fixture->policy_ops;
    service_config.repository = &fixture->repository;
  }
  check_equal(flowie_control_management_service_create(&service_config, service_out), TURBO_OK);
  rpc_config.endpoint = "/v2/control/rpc";
  rpc_config.enable_batch = 0;
  rpc_config.enable_introspection = 0;
  rpc_config.max_batch_size = 0;
  rpc_config.max_request_size = 16384u;
  *rpc_out = rpc_init(&rpc_config);
  check_not_null(*rpc_out);
  server_config.service = *service_out;
  server_config.rpc_context = *rpc_out;
  server_config.resolve_caller = management_rpc_resolve;
  server_config.resolve_caller_ctx = fixture;
  server_config.clock = management_rpc_clock;
  server_config.clock_ctx = fixture;
  server_config.external_https_stats = management_rpc_external_https_stats;
  server_config.external_https_stats_ctx = fixture;
  check_equal(flowie_control_management_rpc_server_create(&server_config, &server), TURBO_OK);
  *app_out = iris_app_create();
  check_not_null(*app_out);
  check_equal(flowie_control_management_rpc_server_bind(server, *app_out), TURBO_OK);
  return server;
}

static void management_rpc_close(flowie_control_management_rpc_server_t *server, rpc_context_t *rpc,
                                 iris_app_t *app, flowie_control_management_service_t *service,
                                 flowie_control_store_t *store, char *path) {
  flowie_control_management_rpc_server_destroy(server);
  check_equal(rpc->method_count, 0u);
  iris_app_destroy(app);
  rpc_destroy(rpc);
  flowie_control_management_service_destroy(service);
  flowie_control_store_destroy(store);
  check_equal(tt_remove_file(path), 0);
  free(path);
}

static turbo_json_doc_t *management_rpc_call(flowie_control_management_rpc_server_t *server,
                                             iris_app_t *app, mem_pool_t *arena,
                                             iris_security_context_t *security, const char *body,
                                             int *status_out) {
  Req request;
  rpc_response_t response;
  turbo_json_doc_t *document = NULL;
  char *response_json = NULL;
  size_t response_size = 0u;
  memset(&request, 0, sizeof(request));
  memset(&response, 0, sizeof(response));
  request.app = app;
  request.arena = arena;
  request.method = "POST";
  request.path = "/v2/control/rpc";
  request.body = (char *)body;
  request.body_len = strlen(body);
  request.security = security;
  *status_out = flowie_control_management_rpc_server_execute(server, &request, &response);
  check_equal(rpc_build_response(&response, &response_json, &response_size), 0);
  check_not_null(response_json);
  check_equal(turbo_parse_json((const uint8_t *)response_json, response_size, &document), TURBO_OK);
  return document;
}

static turbo_json_doc_t *management_rpc_request(flowie_control_management_rpc_server_t *server,
                                                iris_app_t *app, iris_security_context_t *security,
                                                const char *body, int *status_out) {
  turbo_json_doc_t *document;
  mem_pool_t arena;
  check_equal(mem_init(&arena, 0u), 0);
  document = management_rpc_call(server, app, &arena, security, body, status_out);
  mem_destroy(&arena);
  return document;
}

static int management_rpc_error_code(turbo_json_doc_t *document) {
  json_value_t *error = turbo_json_object_get(document, "error");
  return error ? (int)turbo_json_number(turbo_json_object_get(error, "code")) : 0;
}

typedef struct management_rpc_responsiveness_scenario_s {
  flowie_control_management_rpc_server_t *server;
  iris_app_t *app;
  iris_security_context_t security;
  management_rpc_policy_gate_t *gate;
  const char *policy_body;
  int policy_ok;
  int status_ok;
  int status_completed_before_policy;
} management_rpc_responsiveness_scenario_t;

static void management_rpc_policy_task(coro_t *co, void *arg) {
  management_rpc_responsiveness_scenario_t *scenario =
      (management_rpc_responsiveness_scenario_t *)arg;
  turbo_json_doc_t *document = NULL;
  mem_pool_t arena;
  int status = TURBO_EIO;
  (void)co;
  if (mem_init(&arena, 0u) != 0) return;
  document = management_rpc_call(scenario->server, scenario->app, &arena, &scenario->security,
                                 scenario->policy_body, &status);
  scenario->policy_ok = status == TURBO_OK && management_rpc_error_code(document) == 0;
  turbo_free_json(&document);
  mem_destroy(&arena);
}

static void management_rpc_status_task(coro_t *co, void *arg) {
  static const char status_body[] =
      "{\"jsonrpc\":\"2.0\",\"method\":\"control.system.status\",\"id\":3}";
  management_rpc_responsiveness_scenario_t *scenario =
      (management_rpc_responsiveness_scenario_t *)arg;
  turbo_json_doc_t *document = NULL;
  mem_pool_t arena;
  int status = TURBO_EIO;
  (void)co;
  while (!atomic_load_explicit(&scenario->gate->entered, memory_order_acquire))
    coro_sleep(coro_context_current(), 1u);
  if (mem_init(&arena, 0u) == 0) {
    document = management_rpc_call(scenario->server, scenario->app, &arena, &scenario->security,
                                   status_body, &status);
    scenario->status_ok = status == TURBO_OK && management_rpc_error_code(document) == 0;
    turbo_free_json(&document);
    mem_destroy(&arena);
  }
  scenario->status_completed_before_policy =
      !atomic_load_explicit(&scenario->gate->completed, memory_order_acquire);
  atomic_store_explicit(&scenario->gate->status_completed, 1, memory_order_release);
  atomic_store_explicit(&scenario->gate->release, 1, memory_order_release);
}

static void management_rpc_watchdog(void *arg) {
  management_rpc_policy_gate_t *gate = (management_rpc_policy_gate_t *)arg;
  uint64_t deadline = turbo_monotonic_ms() + 750u;
  while (!atomic_load_explicit(&gate->status_completed, memory_order_acquire) &&
         turbo_monotonic_ms() < deadline)
    turbo_sleep_ms(1u);
  atomic_store_explicit(&gate->release, 1, memory_order_release);
}

static void management_rpc_run_responsiveness_scenario(management_rpc_policy_operation_t operation,
                                                       const char *policy_body) {
  char *path = NULL;
  flowie_control_store_t *store = NULL;
  flowie_control_management_service_t *service = NULL;
  flowie_control_management_rpc_server_t *server = NULL;
  rpc_context_t *rpc = NULL;
  iris_app_t *app = NULL;
  management_rpc_fixture_t fixture;
  management_rpc_policy_gate_t gate;
  management_rpc_responsiveness_scenario_t scenario;
  coro_context_t *context;
  turbo_thread_t watchdog = NULL;

  memset(&fixture, 0, sizeof(fixture));
  memset(&gate, 0, sizeof(gate));
  memset(&scenario, 0, sizeof(scenario));
  gate.operation = operation;
  atomic_init(&gate.entered, 0);
  atomic_init(&gate.completed, 0);
  atomic_init(&gate.release, 0);
  atomic_init(&gate.status_completed, 0);
  fixture.caller = (flowie_control_management_caller_t)FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  fixture.caller.domain_id = "root-a";
  fixture.caller.actor = "policy-admin-1";
  fixture.caller.permissions =
      FLOWIE_CONTROL_MANAGEMENT_VIEWER | FLOWIE_CONTROL_MANAGEMENT_POLICY_ADMIN;
  fixture.now = 2000u;
  fixture.policy_gate = &gate;
  atomic_store_explicit(&management_rpc_active_policy_gate, &gate, memory_order_release);
  server = management_rpc_open(&path, &store, &service, &rpc, &app, &fixture);
  context = coro_context_create(NULL);
  check_not_null(context);
  scenario.server = server;
  scenario.app = app;
  scenario.security.authenticated = true;
  scenario.gate = &gate;
  scenario.policy_body = policy_body;
  check_equal(coro_context_spawn(context, management_rpc_policy_task, &scenario), TURBO_OK);
  check_equal(coro_context_spawn(context, management_rpc_status_task, &scenario), TURBO_OK);
  check_equal(turbo_thread_create(&watchdog, management_rpc_watchdog, &gate), TURBO_OK);
  check_equal(coro_context_run(context, TURBO_RUN_DEFAULT), TURBO_OK);
  check_equal(turbo_thread_join(&watchdog), TURBO_OK);
  turbo_thread_destroy(&watchdog);

  check_true(scenario.policy_ok);
  check_true(scenario.status_ok);
  check_true(scenario.status_completed_before_policy);
  coro_context_destroy(context);
  management_rpc_close(server, rpc, app, service, store, path);
  atomic_store_explicit(&management_rpc_active_policy_gate, NULL, memory_order_release);
}

spec("Flowie management JSON-RPC") {
  it("writes structured subject rules within a CoroNet coroutine stack") {
    static const char create_user[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.user.create\",\"params\":{"
        "\"principal_id\":\"tenant-0cddbf38-d8ee-48c1-9165-6fc552c44fb4-tenant-admin-"
        "fdf4f7d0-aec6-448b-9fe6-f0708d253f8a\",\"principal_type\":\"user\","
        "\"request_id\":\"create-coro-policy-user\"},\"id\":1}";
    static const char put_rule[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.subject_rule.put\","
        "\"params\":{\"subject_kind\":\"user\","
        "\"subject_id\":\"tenant-0cddbf38-d8ee-48c1-9165-6fc552c44fb4-tenant-admin-"
        "fdf4f7d0-aec6-448b-9fe6-f0708d253f8a\",\"ordinal\":1025,"
        "\"connection\":\"allow\",\"entries\":["
        "{\"effect\":\"allow\",\"access\":\"write\","
        "\"topic\":\"root-a/groups/beijing/mty/devices/+/camera/command\"},"
        "{\"effect\":\"allow\",\"access\":\"write\","
        "\"topic\":\"root-a/groups/beijing/mty/devices/+/trident/command\"},"
        "{\"effect\":\"allow\",\"access\":\"read\","
        "\"topic\":\"root-a/groups/beijing/mty/devices/+/camera/event\"},"
        "{\"effect\":\"allow\",\"access\":\"read\","
        "\"topic\":\"root-a/groups/beijing/mty/devices/+/trident/event\"}],"
        "\"request_id\":\"put-coro-policy-rule\"},\"id\":2}";
    char *path = NULL;
    flowie_control_store_t *store = NULL;
    flowie_control_management_service_t *service = NULL;
    flowie_control_management_rpc_server_t *server = NULL;
    rpc_context_t *rpc = NULL;
    iris_app_t *app = NULL;
    management_rpc_fixture_t fixture = {FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT, 5000u, TURBO_OK};
    management_rpc_responsiveness_scenario_t scenario;
    iris_security_context_t security = {0};
    turbo_json_doc_t *document = NULL;
    coro_context_t *context = NULL;
    int status = TURBO_EIO;

    memset(&scenario, 0, sizeof(scenario));
    fixture.caller.domain_id = "root-a";
    fixture.caller.actor = "security-admin";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN;
    security.authenticated = true;
    server = management_rpc_open(&path, &store, &service, &rpc, &app, &fixture);
    document = management_rpc_request(server, app, &security, create_user, &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);

    context = coro_context_create(NULL);
    check_not_null(context);
    scenario.server = server;
    scenario.app = app;
    scenario.security = security;
    scenario.policy_body = put_rule;
    check_equal(coro_context_spawn(context, management_rpc_policy_task, &scenario), TURBO_OK);
    check_equal(coro_context_run(context, TURBO_RUN_DEFAULT), TURBO_OK);
    check_true(scenario.policy_ok);

    coro_context_destroy(context);
    management_rpc_close(server, rpc, app, service, store, path);
  }

  it("round-trips structured subject rules and removes every legacy rule method") {
    char *path = NULL;
    flowie_control_store_t *store = NULL;
    flowie_control_management_service_t *service = NULL;
    flowie_control_management_rpc_server_t *server = NULL;
    rpc_context_t *rpc = NULL;
    iris_app_t *app = NULL;
    management_rpc_fixture_t fixture = {FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT, 5000u, TURBO_OK};
    iris_security_context_t security = {0};
    mem_pool_t arena;
    turbo_json_doc_t *document = NULL;
    json_value_t *result;
    json_value_t *entries;
    int status = 0;

    fixture.caller.domain_id = "root-a";
    fixture.caller.actor = "security-admin";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN;
    security.authenticated = true;
    check_equal(mem_init(&arena, 0u), 0);
    server = management_rpc_open(&path, &store, &service, &rpc, &app, &fixture);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.role.create\",\"params\":{"
        "\"role_id\":\"publisher\",\"request_id\":\"create-publisher\"},\"id\":1}",
        &status);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.subject_rule.put\","
                            "\"params\":{\"subject_kind\":\"role\",\"subject_id\":\"publisher\","
                            "\"ordinal\":10,\"connection\":\"allow\",\"entries\":[{"
                            "\"effect\":\"allow\",\"access\":\"write\","
                            "\"topic\":\"root-a/telemetry/%u/event\"}],"
                            "\"request_id\":\"put-publisher-rule\"},\"id\":2}",
                            &status);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.subject_rule.get\","
                            "\"params\":{\"subject_kind\":\"role\",\"subject_id\":\"publisher\"},"
                            "\"id\":3}",
                            &status);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_equal(turbo_json_string(turbo_json_object_get(result, "subject_kind")), "role");
    check_equal(turbo_json_string(turbo_json_object_get(result, "subject_id")), "publisher");
    check_equal((uint64_t)turbo_json_number(turbo_json_object_get(result, "ordinal")), 10u);
    entries = turbo_json_object_get(result, "entries");
    check_equal(turbo_json_array_size(entries), 1u);
    check_equal(
        turbo_json_string(turbo_json_object_get(turbo_json_array_get(entries, 0u), "access")),
        "write");
    turbo_free_json(&document);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.subject_rule.list\","
                            "\"params\":{\"subject_kind\":\"role\",\"limit\":10},\"id\":4}",
                            &status);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_equal(turbo_json_array_size(turbo_json_object_get(result, "items")), 1u);
    turbo_free_json(&document);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.subject_rule.delete\","
        "\"params\":{\"subject_kind\":\"role\",\"subject_id\":\"publisher\","
        "\"request_id\":\"delete-publisher-rule\"},\"id\":5}",
        &status);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.rule.put\","
                            "\"params\":{\"ordinal\":1,\"rule_line\":\"role publisher allow\","
                            "\"request_id\":\"legacy\"},\"id\":6}",
                            &status);
    check_equal(management_rpc_error_code(document), RPC_ERROR_METHOD_NOT_FOUND);
    turbo_free_json(&document);
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.rule.list\",\"id\":7}", &status);
    check_equal(management_rpc_error_code(document), RPC_ERROR_METHOD_NOT_FOUND);
    turbo_free_json(&document);
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.rule.delete\","
        "\"params\":{\"ordinal\":10,\"request_id\":\"legacy-delete\"},\"id\":8}",
        &status);
    check_equal(management_rpc_error_code(document), RPC_ERROR_METHOD_NOT_FOUND);
    turbo_free_json(&document);

    management_rpc_close(server, rpc, app, service, store, path);
    mem_destroy(&arena);
  }

  it("dry-runs structured policy changes without mutating repository state") {
    char *path = NULL;
    flowie_control_store_t *store = NULL;
    flowie_control_management_service_t *service = NULL;
    flowie_control_management_rpc_server_t *server = NULL;
    rpc_context_t *rpc = NULL;
    iris_app_t *app = NULL;
    management_rpc_fixture_t fixture = {FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT, 5000u, TURBO_OK};
    iris_security_context_t security = {0};
    mem_pool_t arena;
    turbo_json_doc_t *document = NULL;
    json_value_t *result = NULL;
    uint64_t revision_before = 0u;
    uint64_t revision_after = 0u;
    size_t audit_before = 0u;
    size_t audit_after = 0u;
    int status = 0;

    fixture.caller.domain_id = "root-a";
    fixture.caller.actor = "policy-admin";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_POLICY_ADMIN;
    security.authenticated = true;
    check_equal(mem_init(&arena, 0u), 0);
    server = management_rpc_open(&path, &store, &service, &rpc, &app, &fixture);

    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN;
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.role.create\",\"params\":{"
        "\"role_id\":\"publisher\",\"request_id\":\"create-publisher\"},\"id\":1}",
        &status);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_POLICY_ADMIN;
    check_equal(flowie_control_store_revision(store, &revision_before), TURBO_OK);
    check_equal(flowie_control_store_audit_count(store, &audit_before), TURBO_OK);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.dry_run\",\"params\":{"
        "\"changes\":[{\"operation\":\"put\",\"subject_kind\":\"role\","
        "\"subject_id\":\"publisher\",\"ordinal\":10,\"connection\":\"allow\","
        "\"entries\":[{\"effect\":\"allow\",\"access\":\"write\","
        "\"topic\":\"root-a/telemetry/%u/event\"}]}]},\"id\":2}",
        &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_not_null(result);
    check_true(turbo_json_bool(turbo_json_object_get(result, "valid")));
    check_equal((uint64_t)turbo_json_number(turbo_json_object_get(result, "store_revision")),
                revision_before);
    check_equal((uint64_t)turbo_json_number(turbo_json_object_get(result, "rule_count")), 2u);
    check_equal(turbo_json_array_size(turbo_json_object_get(result, "diagnostics")), 0u);
    turbo_free_json(&document);

    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_VIEWER;
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.dry_run\",\"params\":{"
        "\"changes\":[{\"operation\":\"delete\",\"subject_kind\":\"role\","
        "\"subject_id\":\"publisher\"}]},\"id\":3}",
        &status);
    check_equal(management_rpc_error_code(document), -32003);
    turbo_free_json(&document);

    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_POLICY_ADMIN;
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.dry_run\",\"params\":{"
        "\"changes\":[{\"operation\":\"put\",\"subject_kind\":\"role\","
        "\"subject_id\":\"missing-role\",\"ordinal\":11,\"connection\":\"allow\","
        "\"entries\":[{\"effect\":\"allow\",\"access\":\"read\","
        "\"topic\":\"root-a/telemetry/%u/event\"}]}]},\"id\":4}",
        &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_false(turbo_json_bool(turbo_json_object_get(result, "valid")));
    check_equal(
        turbo_json_string(turbo_json_object_get(
            turbo_json_array_get(turbo_json_object_get(result, "diagnostics"), 0u), "code")),
        "subject_not_found");
    turbo_free_json(&document);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.dry_run\",\"params\":{"
        "\"changes\":[{\"operation\":\"delete\",\"subject_kind\":\"role\","
        "\"subject_id\":\"publisher\",\"ordinal\":10}]},\"id\":5}",
        &status);
    check_equal(management_rpc_error_code(document), RPC_ERROR_INVALID_PARAMS);
    turbo_free_json(&document);

    check_equal(flowie_control_store_revision(store, &revision_after), TURBO_OK);
    check_equal(flowie_control_store_audit_count(store, &audit_after), TURBO_OK);
    check_equal(revision_after, revision_before);
    check_equal(audit_after, audit_before);

    management_rpc_close(server, rpc, app, service, store, path);
    mem_destroy(&arena);
  }

  it("keeps the CoroNet owner responsive while validating policy") {
    management_rpc_run_responsiveness_scenario(
        MANAGEMENT_RPC_POLICY_VALIDATE,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.validate\",\"id\":1}");
  }

  it("keeps the CoroNet owner responsive while publishing policy") {
    management_rpc_run_responsiveness_scenario(
        MANAGEMENT_RPC_POLICY_PUBLISH,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.publish\",\"params\":{"
        "\"request_id\":\"policy-publish-responsive\"},\"id\":2}");
  }

  it("keeps the CoroNet owner responsive while dry-running policy") {
    management_rpc_run_responsiveness_scenario(
        MANAGEMENT_RPC_POLICY_DRY_RUN,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.policy.dry_run\",\"params\":{"
        "\"changes\":[{\"operation\":\"put\",\"subject_kind\":\"role\","
        "\"subject_id\":\"publisher\",\"ordinal\":10,\"connection\":\"allow\","
        "\"entries\":[{\"effect\":\"allow\",\"access\":\"write\","
        "\"topic\":\"root-a/telemetry/%u/event\"}]}]},\"id\":4}");
  }

  it("binds a dedicated caller-owned context and rejects unsafe RPC forms") {
    char *path = NULL;
    flowie_control_store_t *store = NULL;
    flowie_control_management_service_t *service = NULL;
    flowie_control_management_rpc_server_t *server = NULL;
    rpc_context_t *rpc = NULL;
    iris_app_t *app = NULL;
    management_rpc_fixture_t fixture = {FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT, 5000u, TURBO_OK};
    iris_security_context_t security = {0};
    mem_pool_t arena;
    turbo_json_doc_t *document = NULL;
    int status = 0;

    fixture.caller.domain_id = "root-a";
    fixture.caller.actor = "viewer-1";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_VIEWER;
    check_equal(mem_init(&arena, 0u), 0);
    server = management_rpc_open(&path, &store, &service, &rpc, &app, &fixture);
    check_equal(rpc->method_count, 35u);
    check_equal(iris_app_lookup_rpc_context(app, "/v2/control/rpc"), server);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.system.status\",\"id\":1}", &status);
    check_equal(management_rpc_error_code(document), -32001);
    turbo_free_json(&document);
    security.authenticated = true;
    document = management_rpc_call(server, app, &arena, &security,
                                   "{\"jsonrpc\":\"2.0\",\"method\":\"control.system.status\","
                                   "\"params\":{\"domain_id\":\"root-a\"},\"id\":2}",
                                   &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    check_equal(turbo_json_string(
                    turbo_json_object_get(turbo_json_object_get(document, "result"), "domain")),
                "root-a");
    turbo_free_json(&document);
    document = management_rpc_call(
        server, app, &arena, &security,
        "[{\"jsonrpc\":\"2.0\",\"method\":\"control.system.status\",\"id\":1}]", &status);
    check_equal(management_rpc_error_code(document), RPC_ERROR_INVALID_REQUEST);
    turbo_free_json(&document);
    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.system.status\"}", &status);
    check_equal(management_rpc_error_code(document), RPC_ERROR_INVALID_REQUEST);
    turbo_free_json(&document);
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"flowie.system.status\",\"id\":2}", &status);
    check_equal(management_rpc_error_code(document), RPC_ERROR_METHOD_NOT_FOUND);
    turbo_free_json(&document);
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.group.disable\",\"id\":3}", &status);
    check_equal(management_rpc_error_code(document), RPC_ERROR_METHOD_NOT_FOUND);
    turbo_free_json(&document);
    document = management_rpc_call(server, app, &arena, &security,
                                   "{\"jsonrpc\":\"2.0\",\"method\":\"control.system.status\","
                                   "\"params\":{\"expected_revision\":1},\"id\":3}",
                                   &status);
    check_equal(management_rpc_error_code(document), RPC_ERROR_INVALID_PARAMS);
    turbo_free_json(&document);

    management_rpc_close(server, rpc, app, service, store, path);
    mem_destroy(&arena);
  }

  it("restricts global external HTTPS statistics to the Platform administrator") {
    char *path = NULL;
    flowie_control_store_t *store = NULL;
    flowie_control_management_service_t *service = NULL;
    flowie_control_management_rpc_server_t *server = NULL;
    rpc_context_t *rpc = NULL;
    iris_app_t *app = NULL;
    management_rpc_fixture_t fixture = {FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT, 5000u, TURBO_OK};
    iris_security_context_t security = {0};
    mem_pool_t arena;
    turbo_json_doc_t *document = NULL;
    json_value_t *result = NULL;
    int status = 0;

    fixture.caller.domain_id = "root-a";
    fixture.caller.actor = "viewer-1";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_VIEWER;
    fixture.external_https_enabled = 1;
    fixture.external_https_stats = (flowie_control_external_https_authenticator_stats_t)
        FLOWIE_CONTROL_EXTERNAL_HTTPS_AUTHENTICATOR_STATS_INIT;
    fixture.external_https_stats.started_requests = 17u;
    fixture.external_https_stats.in_flight = 2u;
    fixture.external_https_stats.succeeded = 8u;
    fixture.external_https_stats.denied = 3u;
    fixture.external_https_stats.local_overload = 1u;
    fixture.external_https_stats.remote_overload = 1u;
    fixture.external_https_stats.remote_server_failures = 1u;
    fixture.external_https_stats.transport_failures = 1u;
    fixture.external_https_stats.protocol_failures = 1u;
    fixture.external_https_stats.local_failures = 1u;
    security.authenticated = true;
    check_equal(mem_init(&arena, 0u), 0);
    server = management_rpc_open(&path, &store, &service, &rpc, &app, &fixture);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.auth.external_https.stats\","
                            "\"params\":{\"identity\":\"forbidden\"},\"id\":1}",
                            &status);
    check_equal(status, TURBO_EPROTO);
    check_equal(management_rpc_error_code(document), RPC_ERROR_INVALID_PARAMS);
    check_equal(fixture.external_https_stats_calls, 0u);
    turbo_free_json(&document);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.auth.external_https.stats\",\"id\":2}", &status);
    check_equal(status, TURBO_EPERM);
    check_equal(management_rpc_error_code(document), -32003);
    check_equal(fixture.external_https_stats_calls, 0u);
    turbo_free_json(&document);

    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN;
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.auth.external_https.stats\",\"id\":3}", &status);
    check_equal(status, TURBO_EPERM);
    check_equal(management_rpc_error_code(document), -32003);
    check_equal(fixture.external_https_stats_calls, 0u);
    turbo_free_json(&document);

    fixture.caller.domain_id = FLOWIE_CONTROL_MANAGEMENT_SYSTEM_DOMAIN;
    fixture.caller.actor = "admin";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SYSTEM_ADMIN;
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.auth.external_https.stats\",\"id\":4}", &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_not_null(result);
    check_equal(turbo_json_object_size(result), 11u);
    check_true(turbo_json_bool(turbo_json_object_get(result, "enabled")));
    check_within(turbo_json_number(turbo_json_object_get(result, "started_requests")), 17.0, 0.001);
    check_within(turbo_json_number(turbo_json_object_get(result, "in_flight")), 2.0, 0.001);
    check_within(turbo_json_number(turbo_json_object_get(result, "succeeded")), 8.0, 0.001);
    check_within(turbo_json_number(turbo_json_object_get(result, "denied")), 3.0, 0.001);
    check_within(turbo_json_number(turbo_json_object_get(result, "local_overload")), 1.0, 0.001);
    check_within(turbo_json_number(turbo_json_object_get(result, "remote_overload")), 1.0, 0.001);
    check_within(turbo_json_number(turbo_json_object_get(result, "remote_server_failures")), 1.0,
                 0.001);
    check_within(turbo_json_number(turbo_json_object_get(result, "transport_failures")), 1.0,
                 0.001);
    check_within(turbo_json_number(turbo_json_object_get(result, "protocol_failures")), 1.0, 0.001);
    check_within(turbo_json_number(turbo_json_object_get(result, "local_failures")), 1.0, 0.001);
    check_equal(fixture.external_https_stats_calls, 1u);
    turbo_free_json(&document);

    fixture.external_https_enabled = 0;
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.auth.external_https.stats\",\"params\":{},"
        "\"id\":5}",
        &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_not_null(result);
    check_equal(turbo_json_object_size(result), 1u);
    check_false(turbo_json_bool(turbo_json_object_get(result, "enabled")));
    check_equal(fixture.external_https_stats_calls, 2u);
    turbo_free_json(&document);

    management_rpc_close(server, rpc, app, service, store, path);
    mem_destroy(&arena);
  }

  it("injects root actor and time while enforcing permissions and exact params") {
    char *path = NULL;
    flowie_control_store_t *store = NULL;
    flowie_control_management_service_t *service = NULL;
    flowie_control_management_rpc_server_t *server = NULL;
    rpc_context_t *rpc = NULL;
    iris_app_t *app = NULL;
    management_rpc_fixture_t fixture = {FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT, 5000u, TURBO_OK};
    iris_security_context_t security = {0};
    mem_pool_t arena;
    turbo_json_doc_t *document = NULL;
    json_value_t *result = NULL;
    uint64_t revision = 0u;
    int status = 0;

    fixture.caller.domain_id = "root-a";
    fixture.caller.actor = "admin-1";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_VIEWER;
    security.authenticated = true;
    check_equal(mem_init(&arena, 0u), 0);
    server = management_rpc_open(&path, &store, &service, &rpc, &app, &fixture);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.user.create\",\"params\":{"
                            "\"principal_id\":\"device-1\",\"principal_type\":\"device\","
                            "\"request_id\":\"request-user\"},\"id\":1}",
                            &status);
    check_equal(management_rpc_error_code(document), -32003);
    turbo_free_json(&document);
    fixture.caller.permissions |= FLOWIE_CONTROL_MANAGEMENT_USER_ADMIN;
    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.user.create\",\"params\":{"
                            "\"principal_id\":\"device-1\",\"principal_type\":\"device\","
                            "\"request_id\":\"request-user\","
                            "\"domain\":\"root-b\"},\"id\":2}",
                            &status);
    check_equal(management_rpc_error_code(document), RPC_ERROR_INVALID_PARAMS);
    turbo_free_json(&document);
    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.user.create\",\"params\":{"
                            "\"principal_id\":\"device-1\",\"principal_type\":\"device\","
                            "\"request_id\":\"request-user\"},\"id\":3}",
                            &status);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_not_null(result);
    check_null(turbo_json_object_get(result, "revision"));
    turbo_free_json(&document);
    check_equal(flowie_control_store_revision(store, &revision), TURBO_OK);
    check_equal(revision, 2u);

    management_rpc_close(server, rpc, app, service, store, path);
    mem_destroy(&arena);
  }

  it("returns generated credentials once and enforces secure lifecycle permissions") {
    char *path = NULL;
    flowie_control_store_t *store = NULL;
    flowie_control_management_service_t *service = NULL;
    flowie_control_management_rpc_server_t *server = NULL;
    rpc_context_t *rpc = NULL;
    iris_app_t *app = NULL;
    management_rpc_fixture_t fixture = {FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT, 5000u, TURBO_OK};
    iris_security_context_t security = {0};
    mem_pool_t arena;
    turbo_json_doc_t *document = NULL;
    json_value_t *result = NULL;
    const char *token = NULL;
    char first_token[FLOWIE_CONTROL_CREDENTIAL_TOKEN_CAPACITY] = {0};
    char rotated_token[FLOWIE_CONTROL_CREDENTIAL_TOKEN_CAPACITY] = {0};
    flowie_control_credential_verify_result_t verified =
        FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
    int status = 0;

    fixture.caller.domain_id = "root-a";
    fixture.caller.actor = "user-admin-1";
    fixture.caller.permissions =
        FLOWIE_CONTROL_MANAGEMENT_VIEWER | FLOWIE_CONTROL_MANAGEMENT_USER_ADMIN;
    security.authenticated = true;
    check_equal(mem_init(&arena, 0u), 0);
    server = management_rpc_open(&path, &store, &service, &rpc, &app, &fixture);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.user.create\",\"params\":{"
                            "\"principal_id\":\"device-1\",\"principal_type\":\"device\","
                            "\"request_id\":\"request-user\"},\"id\":1}",
                            &status);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.credential.generate\",\"params\":{"
        "\"principal_id\":\"device-1\",\"request_id\":\"request-generate\""
        "},\"id\":2}",
        &status);
    check_equal(management_rpc_error_code(document), -32003);
    turbo_free_json(&document);

    fixture.caller.actor = "security-admin-1";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN;
    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.credential.generate\",\"params\":{"
        "\"principal_id\":\"device-1\",\"request_id\":\"request-generate\""
        "},\"id\":3}",
        &status);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_not_null(result);
    check_null(turbo_json_object_get(result, "revision"));
    check_null(turbo_json_object_get(result, "secret_base64"));
    token = turbo_json_string(turbo_json_object_get(result, "token"));
    check_not_null(token);
    check_equal(strlen(token), FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE);
    check_starts_with(token, FLOWIE_CONTROL_CREDENTIAL_TOKEN_PREFIX);
    check_null(strchr(token, '+'));
    check_null(strchr(token, '/'));
    check_null(strchr(token, '='));
    memcpy(first_token, token, sizeof(first_token));
    turbo_free_json(&document);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.credential.generate\",\"params\":{"
        "\"principal_id\":\"device-1\",\"request_id\":\"request-generate\""
        "},\"id\":4}",
        &status);
    check_equal(management_rpc_error_code(document), -32010);
    check_null(turbo_json_object_get(document, "result"));
    turbo_free_json(&document);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.credential.rotate\",\"params\":{"
        "\"principal_id\":\"device-1\",\"request_id\":\"request-rotate\""
        "},\"id\":5}",
        &status);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_not_null(result);
    check_null(turbo_json_object_get(result, "revision"));
    check_null(turbo_json_object_get(result, "secret_base64"));
    token = turbo_json_string(turbo_json_object_get(result, "token"));
    check_not_null(token);
    check_equal(strlen(token), FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE);
    check_starts_with(token, FLOWIE_CONTROL_CREDENTIAL_TOKEN_PREFIX);
    check_null(strchr(token, '+'));
    check_null(strchr(token, '/'));
    check_null(strchr(token, '='));
    memcpy(rotated_token, token, sizeof(rotated_token));
    turbo_free_json(&document);

    check_equal(flowie_control_store_credential_verify(store, "root-a", "device-1", first_token,
                                                       FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE,
                                                       &verified),
                TURBO_EPERM);
    verified =
        (flowie_control_credential_verify_result_t)FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
    check_equal(flowie_control_store_credential_verify(store, "root-a", "device-1", rotated_token,
                                                       FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE,
                                                       &verified),
                TURBO_OK);
    check_equal(verified.credential_revision, 4u);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.credential.revoke\",\"params\":{"
        "\"principal_id\":\"device-1\",\"request_id\":\"request-revoke\""
        "},\"id\":6}",
        &status);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_not_null(result);
    check_null(turbo_json_object_get(result, "revision"));
    turbo_free_json(&document);
    check_equal(flowie_control_store_credential_verify(store, "root-a", "device-1", rotated_token,
                                                       FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE,
                                                       &verified),
                TURBO_EPERM);

    flowie_control_credential_wipe(first_token, sizeof(first_token));
    flowie_control_credential_wipe(rotated_token, sizeof(rotated_token));
    management_rpc_close(server, rpc, app, service, store, path);
    mem_destroy(&arena);
  }

  it("syncs a 93-byte service principal with a role and credential") {
    enum { PRINCIPAL_SIZE = 93u, REQUEST_CAPACITY = 1024u };
    char *path = NULL;
    flowie_control_store_t *store = NULL;
    flowie_control_management_service_t *service = NULL;
    flowie_control_management_rpc_server_t *server = NULL;
    rpc_context_t *rpc = NULL;
    iris_app_t *app = NULL;
    management_rpc_fixture_t fixture = {FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT, 5000u, TURBO_OK};
    iris_security_context_t security = {0};
    turbo_json_doc_t *document = NULL;
    json_value_t *result = NULL;
    flowie_control_user_view_t user = FLOWIE_CONTROL_USER_VIEW_INIT;
    flowie_control_effective_roles_view_t roles = FLOWIE_CONTROL_EFFECTIVE_ROLES_VIEW_INIT;
    flowie_control_credential_verify_result_t verified =
        FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
    static const char principal_id[] = "tenant-0cddbf38-d8ee-48c1-9165-6fc552c44fb4-tenant-admin-"
                                       "fdf4f7d0-aec6-448b-9fe6-f0708d253f8a";
    char request[REQUEST_CAPACITY];
    char token[FLOWIE_CONTROL_CREDENTIAL_TOKEN_CAPACITY] = {0};
    const char *generated_token = NULL;
    int written = 0;
    int status = 0;

    check_equal(strlen(principal_id), PRINCIPAL_SIZE);
    fixture.caller.domain_id = "root-a";
    fixture.caller.actor = "security-admin-1";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN;
    security.authenticated = true;
    server = management_rpc_open(&path, &store, &service, &rpc, &app, &fixture);

    written = snprintf(request, sizeof(request),
                       "{\"jsonrpc\":\"2.0\",\"method\":\"control.user.create\",\"params\":{"
                       "\"principal_id\":\"%s\",\"principal_type\":\"service\","
                       "\"request_id\":\"long-principal-create\"},\"id\":1}",
                       principal_id);
    check_true(written > 0 && (size_t)written < sizeof(request));
    document = management_rpc_request(server, app, &security, request, &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);

    document = management_rpc_request(
        server, app, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.role.create\",\"params\":{"
        "\"role_id\":\"tenant-admin\",\"request_id\":\"tenant-admin-create\"},\"id\":2}",
        &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);

    written = snprintf(request, sizeof(request),
                       "{\"jsonrpc\":\"2.0\",\"method\":\"control.role.assign\",\"params\":{"
                       "\"principal_id\":\"%s\",\"role_id\":\"tenant-admin\","
                       "\"request_id\":\"long-principal-role\"},\"id\":3}",
                       principal_id);
    check_true(written > 0 && (size_t)written < sizeof(request));
    document = management_rpc_request(server, app, &security, request, &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);

    written =
        snprintf(request, sizeof(request),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"control.credential.generate\",\"params\":{"
                 "\"principal_id\":\"%s\",\"request_id\":\"long-principal-credential\"},\"id\":4}",
                 principal_id);
    check_true(written > 0 && (size_t)written < sizeof(request));
    document = management_rpc_request(server, app, &security, request, &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    result = turbo_json_object_get(document, "result");
    check_not_null(result);
    generated_token = turbo_json_string(turbo_json_object_get(result, "token"));
    check_not_null(generated_token);
    check_equal(strlen(generated_token), FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE);
    memcpy(token, generated_token, FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE + 1u);
    turbo_free_json(&document);

    check_equal(flowie_control_store_user_get(store, "root-a", principal_id, &user), TURBO_OK);
    check_equal(user.principal_id, principal_id);
    check_equal(user.principal_type, "service");
    check_equal(flowie_control_store_effective_roles(store, "root-a", principal_id, &roles),
                TURBO_OK);
    check_equal(roles.role_count, 1u);
    check_equal(roles.roles[0], "tenant-admin");
    check_equal(flowie_control_store_credential_verify(store, "root-a", principal_id, token,
                                                       FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE,
                                                       &verified),
                TURBO_OK);

    flowie_control_credential_wipe(token, sizeof(token));
    management_rpc_close(server, rpc, app, service, store, path);
  }

  it("lets the platform administrator manage domains without entering them") {
    char *path = NULL;
    flowie_control_store_t *store = NULL;
    flowie_control_management_service_t *service = NULL;
    flowie_control_management_rpc_server_t *server = NULL;
    rpc_context_t *rpc = NULL;
    iris_app_t *app = NULL;
    management_rpc_fixture_t fixture = {FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT, 5000u, TURBO_OK};
    iris_security_context_t security = {0};
    mem_pool_t arena;
    turbo_json_doc_t *document = NULL;
    int status = 0;

    fixture.caller.domain_id = "root-a";
    fixture.caller.actor = "root-admin";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN;
    security.authenticated = true;
    check_equal(mem_init(&arena, 0u), 0);
    server = management_rpc_open(&path, &store, &service, &rpc, &app, &fixture);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.domain.create\",\"params\":{"
                            "\"domain_id\":\"root-b\",\"request_id\":\"root-b-create\""
                            "},\"id\":1}",
                            &status);
    check_equal(management_rpc_error_code(document), -32003);
    turbo_free_json(&document);

    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SYSTEM_ADMIN;
    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.domain.create\",\"params\":{"
                            "\"domain_id\":\"root-b\",\"request_id\":\"root-b-create\""
                            "},\"id\":2}",
                            &status);
    check_equal(management_rpc_error_code(document), -32003);
    turbo_free_json(&document);

    fixture.caller.domain_id = FLOWIE_CONTROL_MANAGEMENT_SYSTEM_DOMAIN;
    fixture.caller.actor = "admin";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SYSTEM_ADMIN;
    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.domain.create\",\"params\":{"
                            "\"domain_id\":\"root-b\",\"request_id\":\"root-b-create\""
                            "},\"id\":3}",
                            &status);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);

    document = management_rpc_call(
        server, app, &arena, &security,
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.domain.admin.initialize\",\"params\":{"
        "\"domain_id\":\"root-b\",\"principal_id\":\"admin-b\","
        "\"initial_password\":\"Root-B-Admin-Password-2026\","
        "\"request_id\":\"root-b-admin-initialize\"},\"id\":4}",
        &status);
    check_equal(status, TURBO_OK);
    check_equal(management_rpc_error_code(document), 0);
    turbo_free_json(&document);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.user.create\",\"params\":{"
                            "\"domain_id\":\"root-b\",\"principal_id\":\"admin-b\","
                            "\"principal_type\":\"human\",\"request_id\":\"admin-b-create\""
                            "},\"id\":3}",
                            &status);
    check_equal(status, TURBO_EPERM);
    check_equal(management_rpc_error_code(document), -32003);
    turbo_free_json(&document);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.password.set\",\"params\":{"
                            "\"domain_id\":\"root-b\",\"principal_id\":\"admin-b\","
                            "\"new_password\":\"Root-B-Admin-Password-2026\",\"mode\":\"create\","
                            "\"request_id\":\"admin-b-password\"},\"id\":4}",
                            &status);
    check_equal(status, TURBO_EPERM);
    check_equal(management_rpc_error_code(document), -32003);
    turbo_free_json(&document);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.domain.list\",\"params\":{"
                            "\"limit\":10},\"id\":8}",
                            &status);
    check_equal(management_rpc_error_code(document), 0);
    {
      json_value_t *rpc_result = turbo_json_object_get(document, "result");
      json_value_t *items = turbo_json_object_get(rpc_result, "items");
      check_equal(turbo_json_array_size(items), 2u);
      check_equal(
          turbo_json_string(turbo_json_object_get(turbo_json_array_get(items, 1u), "domain_id")),
          "root-b");
    }
    turbo_free_json(&document);

    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.user.list\",\"params\":{"
                            "\"domain_id\":\"root-b\",\"limit\":10},\"id\":9}",
                            &status);
    check_equal(status, TURBO_EPERM);
    check_equal(management_rpc_error_code(document), -32003);
    turbo_free_json(&document);

    fixture.caller.domain_id = "root-a";
    fixture.caller.actor = "root-admin";
    fixture.caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN;
    document =
        management_rpc_call(server, app, &arena, &security,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"control.user.list\",\"params\":{"
                            "\"domain_id\":\"root-b\",\"limit\":10},\"id\":10}",
                            &status);
    check_equal(status, TURBO_EPERM);
    check_equal(management_rpc_error_code(document), -32003);
    turbo_free_json(&document);

    management_rpc_close(server, rpc, app, service, store, path);
    mem_destroy(&arena);
  }
}
