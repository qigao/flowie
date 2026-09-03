#include "flowie_control_http_server_internal.h"
#include "flowie_control_rpc_internal.h"
#include "tls_test_support.h"

#include <chttp/chttp.h>
#include "salts_error.h"
#include "tinytest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void flowie_control_http_test_handler(Req *request, Res *response) {
  (void)request;
  send_text(response, OK, "ok");
}

static chttp_client_config flowie_control_http_test_client_config(void) {
  chttp_client_config config = {0};
#if defined(_WIN32)
  config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  config.network.connection_capacity = 2u;
  config.network.command_capacity = 8u;
  config.network.request_capacity = 4u;
  config.network.completion_batch_capacity = 4u;
  config.network.event_capacity = 8u;
  config.network.max_send_bytes = 1024u;
  config.network.receive_buffer_bytes = 1024u;
  config.network.connect_timeout_ms = 5000u;
  config.network.read_timeout_ms = 5000u;
  config.network.write_timeout_ms = 5000u;
  config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  config.network.tls_handshake_timeout_ms = 5000u;
  config.request_capacity = 2u;
  config.max_start_line_bytes = 256u;
  config.max_header_count = 16u;
  config.max_header_bytes = 1024u;
  config.max_request_body_bytes = 512u;
  config.max_response_body_bytes = 512u;
  config.max_informational_responses = 2u;
  return config;
}

spec("Flowie control CHTTP adapter") {
  it("encodes and strictly decodes URL components") {
    char *encoded = flowie_control_http_url_encode("root a/+%~");
    char *decoded;
    check_not_null(encoded);
    check_equal(encoded, "root%20a%2F%2B%25~");
    decoded = flowie_control_http_url_decode(encoded);
    check_not_null(decoded);
    check_equal(decoded, "root a/+%~");
    free(decoded);
    free(encoded);
    check_null(flowie_control_http_url_decode("%00"));
    check_null(flowie_control_http_url_decode("%GG"));
  }

  it("copies response data and rejects header injection") {
    Res response = {0};
    cookie_options_t cookie = {60, "/", "Strict", true, true};
    char body[] = "body";
    set_header(&response, "Cache-Control", "no-store");
    set_cookie(&response, "session", "token", &cookie);
    reply(&response, OK, "text/plain", body, sizeof(body) - 1u);
    body[0] = 'x';
    check_equal(response.status, (unsigned int)OK);
    check_equal(response.body_len, sizeof(body) - 1u);
    check_equal((const char *)response.body, "body");
    check_equal(response.header_count, 2u);
    flowie_control_http_response_clear(&response);

    set_header(&response, "X-Test", "ok\r\nInjected: yes");
    check_equal(response.error, SALTS_EINVAL);
    flowie_control_http_response_clear(&response);
  }

  it("keeps route and context registration bounded and unique") {
    flowie_control_http_app_t *app = flowie_control_http_app_create();
    int marker = 1;
    check_not_null(app);
    check_equal(flowie_control_http_app_bind_context(app, "/rpc", &marker), SALTS_OK);
    check_equal(flowie_control_http_app_bind_context(app, "/rpc", &marker), SALTS_EALREADY);
    check_equal(flowie_control_http_app_lookup_context(app, "/rpc"), &marker);
    check_equal(flowie_control_http_app_post(app, "/rpc", flowie_control_http_test_handler),
                SALTS_OK);
    check_equal(flowie_control_http_app_post(app, "/rpc", flowie_control_http_test_handler),
                SALTS_EALREADY);
    check_equal(flowie_control_http_app_unpost(app, "/rpc", flowie_control_http_test_handler),
                SALTS_OK);
    check_equal(flowie_control_http_app_unpost(app, "/rpc", flowie_control_http_test_handler),
                SALTS_ENOENT);
    check_equal(flowie_control_http_app_post(app, "/rpc", flowie_control_http_test_handler),
                SALTS_OK);
    check_equal(flowie_control_http_app_unbind_context(app, "/rpc", &marker), SALTS_OK);
    flowie_control_http_app_destroy(app);
  }

  it("parses strict JSON-RPC and bounds encoded responses") {
    static const char request_json[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"control.system.status\",\"params\":{},\"id\":7}";
    mem_pool_t arena;
    Req request = {0};
    rpc_request_t rpc_request = {0};
    rpc_response_t response = {0};
    char *output = NULL;
    size_t output_size = 0u;
    memset(&arena, 0, sizeof(arena));
    check_equal(mem_init(&arena, 0u), 0);
    request.arena = &arena;
    request.body = (char *)request_json;
    request.body_len = sizeof(request_json) - 1u;
    check_equal(rpc_parse_request(&request, &rpc_request), 0);
    check_equal(rpc_request.method, "control.system.status");
    check_equal(rpc_request.id, "7");

    response.arena = &arena;
    response.id = rpc_request.id;
    response.result = "{\"ready\":true}";
    response.max_response_size = 128u;
    check_equal(rpc_build_response(&response, &output, &output_size), 0);
    check_not_null(strstr(output, "\"ready\":true"));
    response.max_response_size = 8u;
    check_equal(rpc_build_response(&response, &output, &output_size), -1);
    mem_destroy(&arena);
  }

  it("serves a deferred response over a real CHTTP TLS loopback") {
    flowie_control_http_app_t *app = flowie_control_http_app_create();
    flowie_control_http_tls_config_t server_tls = FLOWIE_CONTROL_HTTP_TLS_CONFIG_INIT;
    cnet_tls_client_config client_tls = {0};
    chttp_tls_profile profile = {0};
    chttp_client client = {0};
    chttp_client_config client_config = flowie_control_http_test_client_config();
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options = {0};
    char cert_path[1024] = {0};
    char key_path[1024] = {0};
    char uri[64];
    uint16_t port = 0u;

    check_not_null(app);
    check_equal(tls_test_write_server_files(cert_path, sizeof(cert_path), key_path,
                                            sizeof(key_path)),
                0);
    server_tls.cert_file = cert_path;
    server_tls.key_file = key_path;
    check_equal(flowie_control_http_app_get(app, "/health", flowie_control_http_test_handler),
                SALTS_OK);
    check_equal(flowie_control_http_app_start_tls(app, "127.0.0.1", 0u, &server_tls), SALTS_OK);
    check_equal(flowie_control_http_app_port(app, &port), SALTS_OK);
    check_true(port != 0u);
    check_true(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port) > 0);

    client_tls.size = sizeof(client_tls);
    client_tls.ca_file = cert_path;
    client_tls.server_name = "localhost";
    check_equal(chttp_tls_profile_init(&profile, &client_tls), SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    options.connection_uri = uri;
    options.authority = "localhost";
    options.target = "/health";
    options.timeout_ms = 5000u;
    options.tls = &profile;
    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body_size, (size_t)2u);
    check_equal(response.body, "ok", 2u);

    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, 5000u), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
    check_equal(flowie_control_http_app_stop(app, 5000u), SALTS_OK);
    flowie_control_http_app_destroy(app);
    tls_test_remove_file(key_path);
    tls_test_remove_file(cert_path);
  }
}
