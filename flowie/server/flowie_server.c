#include "flowie.h"

#include "turbo_error.h"
#include "turbo_parser.h"
#include "turbo_thread.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FLOWIE_SERVER_WAIT_INTERVAL_MS = 100u };

static volatile sig_atomic_t flowie_server_stop_requested = 0;

static void flowie_server_signal(int signal_number) {
  (void)signal_number;
  flowie_server_stop_requested = 1;
}

static int flowie_server_publish(flowie_endpoint_core_t *endpoint, flowie_message_t *message,
                                 flowie_publish_result_t *result, void *ctx) {
  int rc;
  (void)ctx;
  if (!endpoint || !message || !result || result->size < sizeof(*result)) return TURBO_EINVAL;
  rc = flowie_endpoint_core_send_message(endpoint, message);
  result->status = rc;
  if (rc == TURBO_OK) result->protocol_settlement = FLOWIE_PROTOCOL_SETTLE_ACCEPTED;
  return rc;
}

static flowie_transport_t flowie_server_transport(const char *value) {
  if (!value || strcmp(value, "tcp") == 0) return FLOWIE_TRANSPORT_TCP;
  if (strcmp(value, "tls") == 0) return FLOWIE_TRANSPORT_TLS;
  if (strcmp(value, "ws") == 0) return FLOWIE_TRANSPORT_WS;
  if (strcmp(value, "wss") == 0) return FLOWIE_TRANSPORT_WSS;
  return 0;
}

int main(int argc, char **argv) {
  flowie_endpoint_config_t endpoint_config = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
  flowie_endpoint_core_t *endpoint = NULL;
  turbo_cmd_parser_t *parser;
  char *host = NULL;
  char *transport_name = NULL;
  char *path = NULL;
  int64_t port = 1883;
  int64_t max_connections = FLOWIE_DEFAULT_MAX_CONNECTIONS;
  bool check_only = false;
  int rc;

  parser = turbo_cmd_create("flowie_server", "Standalone Flowie MQTT broker");
  if (!parser) {
    (void)fprintf(stderr, "flowie_server: cannot create argument parser\n");
    return EXIT_FAILURE;
  }
  turbo_cmd_add_flag(parser, &check_only, "check", NULL, "Validate options and exit");
  turbo_cmd_add_string(parser, &host, "host", NULL, "Listener host (default: 0.0.0.0)");
  turbo_cmd_add_integer(parser, &port, "port", NULL, "Listener port (default: 1883)");
  turbo_cmd_add_string(parser, &transport_name, "transport", NULL,
                       "Listener transport: tcp, tls, ws, or wss");
  turbo_cmd_add_string(parser, &path, "path", NULL, "WebSocket request path (default: /mqtt)");
  turbo_cmd_add_integer(parser, &max_connections, "max-connections", NULL,
                        "Maximum concurrent MQTT connections");
  turbo_cmd_parse(parser, argc, argv, false);
  turbo_cmd_destroy(parser);

  endpoint_config.transport = flowie_server_transport(transport_name);
  endpoint_config.host = host ? host : "0.0.0.0";
  endpoint_config.port = (int)port;
  endpoint_config.path = path ? path : "/mqtt";
  endpoint_config.max_connections = (uint32_t)max_connections;
  endpoint_config.max_sessions = (size_t)max_connections;
  endpoint_config.manage_sessions = 1;
  options.on_message = flowie_server_publish;
  if (endpoint_config.transport == 0 || port <= 0 || port > UINT16_MAX || max_connections <= 0 ||
      (uint64_t)max_connections > FLOWIE_MAX_CONNECTIONS_LIMIT) {
    (void)fprintf(stderr, "flowie_server: invalid listener options\n");
    return EXIT_FAILURE;
  }

  rc = flowie_endpoint_core_create("mqtt", &endpoint_config, &options, &endpoint);
  if (rc != TURBO_OK) {
    (void)fprintf(stderr, "flowie_server: create failed: %s (%d)\n", turbo_strerror(rc), rc);
    return EXIT_FAILURE;
  }
  if (check_only) {
    flowie_endpoint_core_destroy(endpoint);
    (void)fprintf(stdout, "flowie_server: options are valid\n");
    return EXIT_SUCCESS;
  }
  if (signal(SIGINT, flowie_server_signal) == SIG_ERR ||
      signal(SIGTERM, flowie_server_signal) == SIG_ERR) {
    (void)fprintf(stderr, "flowie_server: cannot install signal handlers\n");
    flowie_endpoint_core_destroy(endpoint);
    return EXIT_FAILURE;
  }
  rc = flowie_endpoint_core_start(endpoint);
  if (rc != TURBO_OK) {
    (void)fprintf(stderr, "flowie_server: start failed: %s (%d)\n", turbo_strerror(rc), rc);
    flowie_endpoint_core_destroy(endpoint);
    return EXIT_FAILURE;
  }
  (void)fprintf(stdout, "flowie_server: mqtt://%s:%d running; press Ctrl+C to stop\n",
                endpoint_config.host, endpoint_config.port);
  while (!flowie_server_stop_requested) turbo_sleep_ms(FLOWIE_SERVER_WAIT_INTERVAL_MS);
  rc = flowie_endpoint_core_stop(endpoint);
  flowie_endpoint_core_destroy(endpoint);
  if (rc != TURBO_OK) {
    (void)fprintf(stderr, "flowie_server: stop failed: %s (%d)\n", turbo_strerror(rc), rc);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
