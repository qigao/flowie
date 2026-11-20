#include <threads/thread_pool.h>
#include <http_client/http.h>
#include <http_server/http.h>
#include <chttp_app/application.h>
#include <stdlib.h>
#include <threads/threads.h>
#include <stdatomic.h>

static atomic_int reject_allocation;
static atomic_int reject_reply;
static atomic_uint reply_count;

static int fault_respond(const chttp_invocation *, unsigned int, const char *, const void *, size_t);
static void *fault_calloc(size_t, size_t);

#define chttp_invocation_respond fault_respond
#define calloc fault_calloc
#include "../src/flowie_control_http_server.c"
#undef calloc
#undef chttp_invocation_respond

#include "tls_test_support.h"
#include "tinytest.h"

enum { FAILURE_TEST_TIMEOUT_MS = 5000, FAILURE_TEST_REQUEST_TIMEOUT_MS = 1000 };

static int fault_respond(const chttp_invocation *invocation, unsigned int status,
                         const char *type, const void *body, size_t size) {
  atomic_fetch_add(&reply_count, 1u);
  if (atomic_exchange(&reject_reply, 0)) return SALTS_ENOBUFS;
  return chttp_invocation_respond(invocation, status, type, body, size);
}

static void *fault_calloc(size_t count, size_t size) {
  if (count == 1u && size == sizeof(flowie_control_http_job_t) &&
      atomic_exchange(&reject_allocation, 0)) return NULL;
  return calloc(count, size);
}

static void failure_test_handler(Req *request, Res *response) {
  (void)request;
  send_text(response, OK, "ok");
}

spec("Application failures release admitted control-plane work") {
  static flowie_control_http_app_t *app;
  static chttp_client client;
  static chttp_tls_profile profile;
  static chttp_response response;
  static chttp_options options;
  static char cert_path[1024];
  static char key_path[1024];
  static char uri[64];

  before_each() {
    flowie_control_http_tls_config_t tls = FLOWIE_CONTROL_HTTP_TLS_CONFIG_INIT;
    cnet_tls_client_config client_tls = {0};
    chttp_client_config config = {0};
    uint16_t port = 0u;
    atomic_store(&reject_allocation, 0);
    atomic_store(&reject_reply, 0);
    atomic_store(&reply_count, 0u);
    app = flowie_control_http_app_create();
    client = (chttp_client){0};
    profile = (chttp_tls_profile){0};
    response = (chttp_response){0};
    options = (chttp_options){0};
    cert_path[0] = key_path[0] = '\0';
    check_not_null(app);
    check_equal(tls_test_write_server_files(cert_path, sizeof(cert_path), key_path,
                                            sizeof(key_path)), 0);
    tls.cert_file = cert_path;
    tls.key_file = key_path;
    check_equal(flowie_control_http_app_get(app, "/health", failure_test_handler), SALTS_OK);
    check_equal(flowie_control_http_app_start_tls(app, "127.0.0.1", 0u, &tls), SALTS_OK);
    check_equal(flowie_control_http_app_port(app, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port) > 0);
    client_tls.size = sizeof(client_tls);
    client_tls.ca_file = cert_path;
    client_tls.server_name = "localhost";
    check_equal(chttp_tls_profile_init(&profile, &client_tls), SALTS_OK);
    config.network.backend = flowie_control_http_native_backend();
    config.network.connection_capacity = 2u;
    config.network.command_capacity = 8u;
    config.network.request_capacity = 4u;
    config.network.completion_batch_capacity = 4u;
    config.network.event_capacity = 8u;
    config.network.max_send_bytes = 1024u;
    config.network.receive_buffer_bytes = 1024u;
    config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    config.network.tls_handshake_timeout_ms = FAILURE_TEST_TIMEOUT_MS;
    config.request_capacity = 2u;
    config.max_start_line_bytes = 256u;
    config.max_header_count = 16u;
    config.max_header_bytes = 1024u;
    config.max_request_body_bytes = 512u;
    config.max_response_body_bytes = 512u;
    config.max_informational_responses = 2u;
    check_equal(chttp_client_init(&client, &config), SALTS_OK);
    options.connection_uri = uri;
    options.authority = "localhost";
    options.target = "/health";
    options.timeout_ms = FAILURE_TEST_REQUEST_TIMEOUT_MS;
    options.tls = &profile;
  }

  after_each() {
    chttp_response_destroy(&response);
    if (client.impl) check_equal(chttp_client_destroy(&client, FAILURE_TEST_TIMEOUT_MS), SALTS_OK);
    if (profile.impl) check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
    if (app) {
      check_equal(flowie_control_http_app_stop(app, FAILURE_TEST_TIMEOUT_MS), SALTS_OK);
      flowie_control_http_app_destroy(app);
      app = NULL;
    }
    if (key_path[0]) tls_test_remove_file(key_path);
    if (cert_path[0]) tls_test_remove_file(cert_path);
  }


  it("propagates response failure without stranding Application shutdown") {
    chttp_error error = {0};
    atomic_store(&reject_reply, 1);
    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, (unsigned int)INTERNAL_SERVER_ERROR);
    check_equal(atomic_load(&reply_count), 1u);
    chttp_response_destroy(&response);
    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, (unsigned int)OK);
  }

  it("rejects a failed mutable request copy and continues serving") {
    chttp_error error = {0};
    atomic_store(&reject_allocation, 1);
    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, (unsigned int)SERVICE_UNAVAILABLE);
    chttp_response_destroy(&response);
    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, (unsigned int)OK);
  }
}
