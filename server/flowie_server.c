#include "flowie.h"
#include "flowie_server_turbodb_config_internal.h"

#include "turbo_error.h"
#include "turbo_parser.h"
#include "turbo_thread.h"
#include "tlog.h"

#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_SERVER_WAIT_INTERVAL_MS = 100u,
  FLOWIE_SERVER_LOG_BUFFER_BYTES = 64u * 1024u,
  FLOWIE_SERVER_LOG_POOL_BYTES = 32u * 1024u
};

#define FLOWIE_SERVER_LOG_COMPONENT "Flowie.Server"
#define FLOWIE_SERVER_PROTOCOL_NAMESPACE "flowie_server"
#define FLOWIE_SERVER_PROTOCOL_STORE_DRIVER_ENV "FLOWIE_PROTOCOL_STORE_DRIVER"
#define FLOWIE_SERVER_PROTOCOL_STORE_OPTIONS_ENV "FLOWIE_PROTOCOL_STORE_OPTIONS"

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

static int flowie_server_log_level(const char *value, turbo_log_level_t *level) {
  if (!level) return TURBO_EINVAL;
  if (!value || strcmp(value, "INFO") == 0 || strcmp(value, "info") == 0) {
    *level = TURBO_LOG_LEVEL_INFO;
    return TURBO_OK;
  }
  if (strcmp(value, "DEBUG") == 0 || strcmp(value, "debug") == 0)
    *level = TURBO_LOG_LEVEL_DEBUG;
  else if (strcmp(value, "WARN") == 0 || strcmp(value, "warn") == 0)
    *level = TURBO_LOG_LEVEL_WARN;
  else if (strcmp(value, "ERROR") == 0 || strcmp(value, "error") == 0)
    *level = TURBO_LOG_LEVEL_ERROR;
  else if (strcmp(value, "FATAL") == 0 || strcmp(value, "fatal") == 0)
    *level = TURBO_LOG_LEVEL_FATAL;
  else
    return TURBO_EINVAL;
  return TURBO_OK;
}

typedef struct flowie_server_tuning_s {
  int64_t max_packet_size;
  int64_t max_connections;
  int64_t max_sessions;
  int64_t max_subscriptions_per_session;
  int64_t max_inflight_per_session;
  int64_t max_retained_messages;
  int64_t send_hwm_bytes;
  int64_t coroutine_stack_size;
  int64_t stream_recv_buffer_bytes;
  int64_t socket_recv_buffer_bytes;
  int64_t socket_send_buffer_bytes;
  int64_t timeout_ms;
  int64_t recv_timeout_ms;
  int64_t tcp_keepalive_idle_ms;
  int64_t tcp_keepalive_interval_ms;
  int64_t tcp_keepalive_count;
  bool tcp_keepalive;
  bool reuse_port;
} flowie_server_tuning_t;

#define FLOWIE_SERVER_TUNING_INIT                                                               \
  {                                                                                             \
    FLOWIE_DEFAULT_MAX_PACKET_SIZE, FLOWIE_DEFAULT_MAX_CONNECTIONS, 0,                          \
        FLOWIE_DEFAULT_MAX_SUBSCRIPTIONS_PER_SESSION, FLOWIE_DEFAULT_MAX_INFLIGHT_PER_SESSION,  \
        0, FLOWIE_DEFAULT_SEND_HWM_BYTES, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, false               \
  }

static int flowie_server_tuning_apply(const flowie_server_tuning_t *tuning,
                                      flowie_endpoint_config_t *config) {
  uint64_t max_sessions;
  uint64_t max_retained_messages;
  if (!tuning || !config) return TURBO_EINVAL;
  if (tuning->max_packet_size < 2 ||
      (uint64_t)tuning->max_packet_size > FLOWIE_MQTT_MAX_WIRE_PACKET_SIZE ||
      tuning->max_connections <= 0 ||
      (uint64_t)tuning->max_connections > FLOWIE_MAX_CONNECTIONS_LIMIT ||
      tuning->max_sessions < 0 || tuning->max_subscriptions_per_session <= 0 ||
      tuning->max_subscriptions_per_session > UINT16_MAX ||
      tuning->max_inflight_per_session <= 0 || tuning->max_inflight_per_session > UINT16_MAX ||
      tuning->max_retained_messages < 0 || tuning->send_hwm_bytes <= 0)
    return TURBO_ERANGE;
  if ((tuning->coroutine_stack_size != 0 &&
       (tuning->coroutine_stack_size < FLOWIE_MIN_COROUTINE_STACK_SIZE ||
        tuning->coroutine_stack_size > FLOWIE_MAX_COROUTINE_STACK_SIZE)) ||
      (tuning->stream_recv_buffer_bytes != 0 &&
       (tuning->stream_recv_buffer_bytes < FLOWIE_MIN_RECV_BUFFER_SIZE ||
        tuning->stream_recv_buffer_bytes > FLOWIE_MAX_RECV_BUFFER_SIZE)) ||
      tuning->socket_recv_buffer_bytes < 0 || tuning->socket_recv_buffer_bytes > INT_MAX ||
      tuning->socket_send_buffer_bytes < 0 || tuning->socket_send_buffer_bytes > INT_MAX ||
      tuning->timeout_ms < 0 || tuning->recv_timeout_ms < 0 ||
      tuning->tcp_keepalive_idle_ms < 0 || tuning->tcp_keepalive_idle_ms > UINT32_MAX ||
      tuning->tcp_keepalive_interval_ms < 0 ||
      tuning->tcp_keepalive_interval_ms > UINT32_MAX || tuning->tcp_keepalive_count < 0 ||
      (uint64_t)tuning->tcp_keepalive_count > UINT32_MAX)
    return TURBO_ERANGE;
  if (!tuning->tcp_keepalive &&
      (tuning->tcp_keepalive_idle_ms != 0 || tuning->tcp_keepalive_interval_ms != 0 ||
       tuning->tcp_keepalive_count != 0))
    return TURBO_EINVAL;
  max_sessions = tuning->max_sessions == 0 ? (uint64_t)tuning->max_connections
                                           : (uint64_t)tuning->max_sessions;
  max_retained_messages = tuning->max_retained_messages == 0
                              ? max_sessions
                              : (uint64_t)tuning->max_retained_messages;
  if (max_sessions > SIZE_MAX || max_retained_messages > SIZE_MAX) return TURBO_ERANGE;
  config->max_packet_size = (size_t)tuning->max_packet_size;
  config->max_connections = (uint32_t)tuning->max_connections;
  config->max_sessions = (size_t)max_sessions;
  config->max_subscriptions_per_session = (size_t)tuning->max_subscriptions_per_session;
  config->max_inflight_per_session = (size_t)tuning->max_inflight_per_session;
  config->max_retained_messages = (size_t)max_retained_messages;
  config->send_hwm_bytes = (size_t)tuning->send_hwm_bytes;
  config->coroutine_stack_size = (size_t)tuning->coroutine_stack_size;
  config->stream_recv_buffer_bytes = (size_t)tuning->stream_recv_buffer_bytes;
  config->socket_recv_buffer_bytes = (size_t)tuning->socket_recv_buffer_bytes;
  config->socket_send_buffer_bytes = (size_t)tuning->socket_send_buffer_bytes;
  config->timeout_ms = (uint64_t)tuning->timeout_ms;
  config->recv_timeout_ms = (uint64_t)tuning->recv_timeout_ms;
  config->tcp_keepalive = tuning->tcp_keepalive ? 1 : 0;
  config->tcp_keepalive_idle_ms = (uint64_t)tuning->tcp_keepalive_idle_ms;
  config->tcp_keepalive_interval_ms = (uint64_t)tuning->tcp_keepalive_interval_ms;
  config->tcp_keepalive_count = (uint32_t)tuning->tcp_keepalive_count;
  config->reuse_port = tuning->reuse_port ? 1 : 0;
  return TURBO_OK;
}

static tlog_t *flowie_server_logging_create(turbo_log_level_t level) {
  tlog_config_t config = {.min_level = level,
                          .buffer_size = FLOWIE_SERVER_LOG_BUFFER_BYTES,
                          .pool_size = FLOWIE_SERVER_LOG_POOL_BYTES};
  turbo_console_sink_opts_t console_options = {
      .output = stderr, .use_colors = 0, .pattern = TURBO_LOG_FULL_PATTERN};
  tlog_t *logger = tlog_create(&config);
  turbo_log_sink_t *sink;
  if (!logger) return NULL;
  sink = turbo_sink_console_create(&console_options);
  if (!sink || tlog_add_sink(logger, sink) != TURBO_OK) {
    turbo_sink_destroy(sink);
    tlog_destroy(logger);
    return NULL;
  }
  tlog_set_default(logger);
  return logger;
}

static void flowie_server_logging_destroy(tlog_t *logger) {
  unsigned long long written_before_summary;
  unsigned long long dropped_total;
  size_t pending_before_summary;

  if (!logger) return;
  tlog_flush(logger);
  written_before_summary = (unsigned long long)tlog_get_written(logger);
  dropped_total = (unsigned long long)tlog_get_dropped(logger);
  pending_before_summary = tlog_get_queue_size(logger);
  TURBO_LOG_INFOF(logger, FLOWIE_SERVER_LOG_COMPONENT,
                  "logging-shutdown written_before_summary={} dropped_total={} "
                  "pending_before_summary={}",
                  written_before_summary, dropped_total, pending_before_summary);
  tlog_flush(logger);
  tlog_set_default(NULL);
  tlog_destroy(logger);
}

static int flowie_server_repository_open(const orm_config_t *database,
                                         const flowie_endpoint_config_t *endpoint_config,
                                         flowie_protocol_repository_t **out) {
  flowie_protocol_repository_config_t repository_config = FLOWIE_PROTOCOL_REPOSITORY_CONFIG_INIT;
  if (out) *out = NULL;
  if (!database || !endpoint_config || !out) return TURBO_EINVAL;

  repository_config.database = database;
  repository_config.namespace_name = FLOWIE_SERVER_PROTOCOL_NAMESPACE;
  repository_config.create_schema = 1;
  repository_config.limits.max_sessions = endpoint_config->max_sessions;
  repository_config.limits.max_subscriptions_per_session =
      endpoint_config->max_subscriptions_per_session;
  repository_config.limits.max_inflight_per_session = endpoint_config->max_inflight_per_session;
  repository_config.limits.max_retained_messages = endpoint_config->max_retained_messages;
  repository_config.limits.max_client_id_size = FLOWIE_MQTT_MAX_UTF8_SIZE;
  repository_config.limits.max_topic_size = FLOWIE_MQTT_MAX_UTF8_SIZE;
  repository_config.limits.max_packet_size = endpoint_config->max_packet_size;
  return flowie_protocol_repository_open(&repository_config, out);
}

static void flowie_server_runtime_destroy(flowie_endpoint_core_t *endpoint,
                                          flowie_protocol_repository_t *repository) {
  flowie_endpoint_core_destroy(endpoint);
  flowie_protocol_repository_close(repository);
}

int main(int argc, char **argv) {
  flowie_endpoint_config_t endpoint_config = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
  const flowie_execution_binding_t execution = {
      sizeof(flowie_execution_binding_t), FLOWIE_EXECUTION_PRIVATE, NULL, NULL, 0u, 0u};
  flowie_endpoint_persistence_binding_t persistence = FLOWIE_ENDPOINT_PERSISTENCE_BINDING_INIT;
  flowie_endpoint_bindings_t bindings = FLOWIE_ENDPOINT_BINDINGS_INIT;
  flowie_endpoint_core_t *endpoint = NULL;
  flowie_protocol_repository_t *repository = NULL;
  flowie_server_turbodb_config_t *turbodb = NULL;
  turbo_cmd_parser_t *parser;
  char *host = NULL;
  char *transport_name = NULL;
  char *path = NULL;
  char *protocol_store_driver = NULL;
  char *protocol_store_options = NULL;
  char *log_level_name = NULL;
  int64_t port = 1883;
  flowie_server_tuning_t tuning = FLOWIE_SERVER_TUNING_INIT;
  bool check_only = false;
  turbo_log_level_t log_level = TURBO_LOG_LEVEL_INFO;
  tlog_t *logger = NULL;
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
  turbo_cmd_add_string(parser, &protocol_store_driver, "protocol-store-driver", NULL,
                       "Configured TurboDB driver (default: sqlite)");
  turbo_cmd_set_env(parser, turbo_cmd_last_index(parser), FLOWIE_SERVER_PROTOCOL_STORE_DRIVER_ENV);
  turbo_cmd_add_string(parser, &protocol_store_options, "protocol-store-options", NULL,
                       "JSON object of configured TurboDB string options");
  turbo_cmd_set_env(parser, turbo_cmd_last_index(parser), FLOWIE_SERVER_PROTOCOL_STORE_OPTIONS_ENV);
  turbo_cmd_add_integer(parser, &tuning.max_packet_size, "max-packet-size", NULL,
                        "Maximum MQTT wire packet bytes");
  turbo_cmd_add_integer(parser, &tuning.max_connections, "max-connections", NULL,
                        "Maximum concurrent MQTT connections");
  turbo_cmd_add_integer(parser, &tuning.max_sessions, "max-sessions", NULL,
                        "Maximum managed sessions; 0 follows max-connections");
  turbo_cmd_add_integer(parser, &tuning.max_subscriptions_per_session,
                        "max-subscriptions-per-session", NULL,
                        "Maximum subscriptions per MQTT session");
  turbo_cmd_add_integer(parser, &tuning.max_inflight_per_session, "max-inflight", NULL,
                        "Maximum outbound QoS messages in flight per MQTT session");
  turbo_cmd_add_integer(parser, &tuning.max_retained_messages, "max-retained-messages", NULL,
                        "Maximum retained messages; 0 follows max-sessions");
  turbo_cmd_add_integer(parser, &tuning.send_hwm_bytes, "send-hwm-bytes", NULL,
                        "Per-connection pending-send byte high-water mark");
  turbo_cmd_add_integer(parser, &tuning.coroutine_stack_size, "coroutine-stack-size", NULL,
                        "Private-context coroutine stack bytes; 0 uses component default");
  turbo_cmd_add_integer(parser, &tuning.stream_recv_buffer_bytes, "stream-recv-buffer-bytes", NULL,
                        "Bytes in each private-context receive chunk; 0 uses component default");
  turbo_cmd_add_integer(parser, &tuning.socket_recv_buffer_bytes, "socket-recv-buffer-bytes", NULL,
                        "Requested OS receive-buffer bytes; 0 preserves OS default");
  turbo_cmd_add_integer(parser, &tuning.socket_send_buffer_bytes, "socket-send-buffer-bytes", NULL,
                        "Requested OS send-buffer bytes; 0 preserves OS default");
  turbo_cmd_add_integer(parser, &tuning.timeout_ms, "timeout-ms", NULL,
                        "Default socket timeout milliseconds; 0 disables the default timeout");
  turbo_cmd_add_integer(parser, &tuning.recv_timeout_ms, "recv-timeout-ms", NULL,
                        "Receive timeout milliseconds; 0 follows timeout-ms");
  turbo_cmd_add_flag(parser, &tuning.tcp_keepalive, "tcp-keepalive", NULL,
                     "Enable TCP keepalive for accepted connections");
  turbo_cmd_add_integer(parser, &tuning.tcp_keepalive_idle_ms, "tcp-keepalive-idle-ms", NULL,
                        "TCP keepalive idle milliseconds; requires --tcp-keepalive");
  turbo_cmd_add_integer(parser, &tuning.tcp_keepalive_interval_ms,
                        "tcp-keepalive-interval-ms", NULL,
                        "TCP keepalive interval milliseconds; requires --tcp-keepalive");
  turbo_cmd_add_integer(parser, &tuning.tcp_keepalive_count, "tcp-keepalive-count", NULL,
                        "TCP keepalive probe count; requires --tcp-keepalive");
  turbo_cmd_add_flag(parser, &tuning.reuse_port, "reuse-port", NULL,
                     "Enable listener port reuse");
  turbo_cmd_add_string(parser, &log_level_name, "log-level", NULL,
                       "Log level: DEBUG, INFO, WARN, ERROR, or FATAL");
  turbo_cmd_parse(parser, argc, argv, false);
  turbo_cmd_destroy(parser);

  endpoint_config.transport = flowie_server_transport(transport_name);
  endpoint_config.host = host ? host : "0.0.0.0";
  endpoint_config.port = (int)port;
  endpoint_config.path = path ? path : "/mqtt";
  endpoint_config.manage_sessions = 1;
  protocol_store_driver = protocol_store_driver ? protocol_store_driver
                                                : FLOWIE_SERVER_TURBODB_DEFAULT_DRIVER;
  protocol_store_options = protocol_store_options ? protocol_store_options
                                                  : FLOWIE_SERVER_TURBODB_DEFAULT_OPTIONS;
  options.on_message = flowie_server_publish;
  if (endpoint_config.transport == 0 || port <= 0 || port > UINT16_MAX ||
      flowie_server_tuning_apply(&tuning, &endpoint_config) != TURBO_OK) {
    (void)fprintf(stderr, "flowie_server: invalid listener options\n");
    return EXIT_FAILURE;
  }
  if (flowie_server_log_level(log_level_name, &log_level) != TURBO_OK) {
    (void)fprintf(stderr, "flowie_server: invalid log level: %s\n",
                  log_level_name ? log_level_name : "(null)");
    return EXIT_FAILURE;
  }
  rc = flowie_server_turbodb_config_create(protocol_store_driver, protocol_store_options, &turbodb);
  if (rc != TURBO_OK) {
    (void)fprintf(stderr, "flowie_server: invalid TurboDB configuration: %s\n",
                  turbo_strerror(rc));
    return EXIT_FAILURE;
  }
  logger = flowie_server_logging_create(log_level);
  if (!logger) {
    (void)fprintf(stderr, "flowie_server: cannot initialize logging\n");
    flowie_server_turbodb_config_destroy(turbodb);
    return EXIT_FAILURE;
  }
  TURBO_LOG_DEBUGF(logger, FLOWIE_SERVER_LOG_COMPONENT,
                   "effective-config transport={} host={} port={} path={} reuse_port={} "
                   "protocol_store_driver={} log_level={}",
                   transport_name ? transport_name : "tcp", endpoint_config.host,
                   endpoint_config.port, endpoint_config.path, endpoint_config.reuse_port,
                   flowie_server_turbodb_config_driver(turbodb), turbo_log_level_name(log_level));
  TURBO_LOG_DEBUGF(
      logger, FLOWIE_SERVER_LOG_COMPONENT,
      "effective-config max_packet_size={} max_connections={} max_sessions={} "
      "max_subscriptions_per_session={} max_inflight_per_session={} max_retained_messages={} "
      "send_hwm_bytes={}",
      (unsigned long long)endpoint_config.max_packet_size,
      (unsigned int)endpoint_config.max_connections,
      (unsigned long long)endpoint_config.max_sessions,
      (unsigned long long)endpoint_config.max_subscriptions_per_session,
      (unsigned long long)endpoint_config.max_inflight_per_session,
      (unsigned long long)endpoint_config.max_retained_messages,
      (unsigned long long)endpoint_config.send_hwm_bytes);
  TURBO_LOG_DEBUGF(
      logger, FLOWIE_SERVER_LOG_COMPONENT,
      "effective-config coroutine_stack_size={} stream_recv_buffer_bytes={} "
      "socket_recv_buffer_bytes={} socket_send_buffer_bytes={} timeout_ms={} recv_timeout_ms={} "
      "tcp_keepalive={} tcp_keepalive_idle_ms={} tcp_keepalive_interval_ms={} "
      "tcp_keepalive_count={}",
      (unsigned long long)endpoint_config.coroutine_stack_size,
      (unsigned long long)endpoint_config.stream_recv_buffer_bytes,
      (unsigned long long)endpoint_config.socket_recv_buffer_bytes,
      (unsigned long long)endpoint_config.socket_send_buffer_bytes,
      (unsigned long long)endpoint_config.timeout_ms,
      (unsigned long long)endpoint_config.recv_timeout_ms, endpoint_config.tcp_keepalive,
      (unsigned long long)endpoint_config.tcp_keepalive_idle_ms,
      (unsigned long long)endpoint_config.tcp_keepalive_interval_ms,
      (unsigned int)endpoint_config.tcp_keepalive_count);

  rc = flowie_server_repository_open(flowie_server_turbodb_config_database(turbodb),
                                     &endpoint_config, &repository);
  if (rc != TURBO_OK) {
    TURBO_LOG_ERRORF(logger, FLOWIE_SERVER_LOG_COMPONENT,
                     "protocol-store-open-failed driver={} status={} reason={}",
                     flowie_server_turbodb_config_driver(turbodb), rc, turbo_strerror(rc));
    flowie_server_logging_destroy(logger);
    flowie_server_turbodb_config_destroy(turbodb);
    return EXIT_FAILURE;
  }
  persistence.repository = repository;
  bindings.persistence = &persistence;
  rc = flowie_endpoint_core_create_ex("mqtt", &endpoint_config, &options, &execution, &bindings,
                                      &endpoint);
  if (rc != TURBO_OK) {
    TURBO_LOG_ERRORF(logger, FLOWIE_SERVER_LOG_COMPONENT,
                     "endpoint-create-failed status={} reason={}", rc, turbo_strerror(rc));
    flowie_server_runtime_destroy(endpoint, repository);
    flowie_server_logging_destroy(logger);
    flowie_server_turbodb_config_destroy(turbodb);
    return EXIT_FAILURE;
  }
  if (check_only) {
    flowie_server_runtime_destroy(endpoint, repository);
    (void)fprintf(stdout, "flowie_server: options are valid\n");
    flowie_server_logging_destroy(logger);
    flowie_server_turbodb_config_destroy(turbodb);
    return EXIT_SUCCESS;
  }
  if (signal(SIGINT, flowie_server_signal) == SIG_ERR ||
      signal(SIGTERM, flowie_server_signal) == SIG_ERR) {
    TURBO_LOG_ERROR(logger, FLOWIE_SERVER_LOG_COMPONENT,
                    "signal-handler-install-failed action=check process signal policy");
    flowie_server_runtime_destroy(endpoint, repository);
    flowie_server_logging_destroy(logger);
    flowie_server_turbodb_config_destroy(turbodb);
    return EXIT_FAILURE;
  }
  rc = flowie_endpoint_core_start(endpoint);
  if (rc != TURBO_OK) {
    TURBO_LOG_ERRORF(logger, FLOWIE_SERVER_LOG_COMPONENT,
                     "endpoint-start-failed status={} reason={}", rc, turbo_strerror(rc));
    flowie_server_runtime_destroy(endpoint, repository);
    flowie_server_logging_destroy(logger);
    flowie_server_turbodb_config_destroy(turbodb);
    return EXIT_FAILURE;
  }
  TURBO_LOG_INFOF(logger, FLOWIE_SERVER_LOG_COMPONENT,
                  "server-started transport={} host={} port={}",
                  transport_name ? transport_name : "tcp", endpoint_config.host,
                  endpoint_config.port);
  (void)fprintf(stdout, "flowie_server: mqtt://%s:%d running; press Ctrl+C to stop\n",
                endpoint_config.host, endpoint_config.port);
  while (!flowie_server_stop_requested) turbo_sleep_ms(FLOWIE_SERVER_WAIT_INTERVAL_MS);
  rc = flowie_endpoint_core_stop(endpoint);
  flowie_server_runtime_destroy(endpoint, repository);
  if (rc != TURBO_OK) {
    TURBO_LOG_ERRORF(logger, FLOWIE_SERVER_LOG_COMPONENT,
                     "endpoint-stop-failed status={} reason={}", rc, turbo_strerror(rc));
    flowie_server_logging_destroy(logger);
    flowie_server_turbodb_config_destroy(turbodb);
    return EXIT_FAILURE;
  }
  TURBO_LOG_INFO(logger, FLOWIE_SERVER_LOG_COMPONENT, "server-stopped status=0");
  flowie_server_logging_destroy(logger);
  flowie_server_turbodb_config_destroy(turbodb);
  return EXIT_SUCCESS;
}
