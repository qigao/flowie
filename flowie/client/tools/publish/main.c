#include "flowie_mqtt_client.h"

#include <core/error.h>
#include <core/platform.h>
#include <threads/threads.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

enum { payload_limit = 4096, command_timeout_ms = 2500, publish_timeout_ms = 5500 };

typedef struct {
  sync_mutex_t mutex;
  sync_cond_t ready;
  int finished;
  int status;
  int reason;
  flowie_mqtt_client_publish_topic_t topic;
} command_state;

static flowie_mqtt_span_t span(const char *text) {
  return (flowie_mqtt_span_t){(const uint8_t *)text, strlen(text)};
}

static void complete(command_state *state, int status, int reason) {
  sync_mutex_lock(&state->mutex);
  state->status = status;
  state->reason = reason;
  state->finished = 1;
  sync_cond_signal(&state->ready);
  sync_mutex_unlock(&state->mutex);
}

static void on_publish(flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *reply, void *context) {
  (void)client;
  if (status == SALTS_OK && (!reply || reply->type != FLOWIE_MQTT_PACKET_PUBACK)) status = SALTS_EPROTO;
  complete(context, status, status == SALTS_OK ? reply->reason_code : -1);
}

static void on_connect(flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *reply, void *context) {
  command_state *state = context;
  if (status == SALTS_OK && (!reply || reply->type != FLOWIE_MQTT_PACKET_CONNACK || reply->reason_code != 0u))
    status = SALTS_ECONNREFUSED;
  if (status == SALTS_OK) {
    flowie_mqtt_client_publish_topic_vec_t request = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    request.count = 1;
    request.data = &state->topic;
    status = flowie_mqtt_client_publish(client, &request);
  }
  if (status != SALTS_OK) complete(state, status, -1);
}

static int wait_for_result(command_state *state, int *reason) {
  const uint64_t deadline = monotonic_ms() + publish_timeout_ms;
  sync_mutex_lock(&state->mutex);
  while (!state->finished) {
    const uint64_t now = monotonic_ms();
    if (now >= deadline || sync_cond_timedwait(&state->ready, &state->mutex,
        (deadline - now) * UINT64_C(1000000)) != SALTS_OK) break;
  }
  const int status = state->finished ? state->status : SALTS_ETIMEDOUT;
  *reason = state->finished ? state->reason : -1;
  sync_mutex_unlock(&state->mutex);
  return status;
}

int main(int argc, char **argv) {
  if (argc != 9) {
    fputs("Usage: flowie_mqtt_publish host port ca.pem cert.pem key.pem principal client-id topic < payload\n", stderr);
    return EXIT_FAILURE;
  }
  char *end;
  const long port = strtol(argv[2], &end, 10);
  if (end == argv[2] || *end || port <= 0 || port > 65535) return EXIT_FAILURE;
#ifdef _WIN32
  if (_setmode(_fileno(stdin), _O_BINARY) == -1) return EXIT_FAILURE;
#endif
  uint8_t payload[payload_limit + 1];
  const size_t length = fread(payload, 1, sizeof(payload), stdin);
  if (ferror(stdin) || length == 0 || length > payload_limit) {
    fputs("Expected a payload of 1 to 4096 bytes\n", stderr);
    return EXIT_FAILURE;
  }

  command_state state = {0};
  state.topic.qos = 1;
  state.topic.topic = span(argv[8]);
  state.topic.payload = (flowie_mqtt_span_t){payload, length};
  sync_mutex_init(&state.mutex);
  sync_cond_init(&state.ready);

  flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  config.host = argv[1];
  config.port = (int)port;
  config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
  config.timeout_ms = command_timeout_ms;
  config.tls = (flowie_mqtt_client_tls_config_t){argv[3], argv[4], argv[5], NULL};
  config.user_data = &state;
  config.on_connect = on_connect;
  config.on_publish = on_publish;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  connect.clean_start = 1;
  connect.has_username = 1;
  connect.keep_alive = 15;
  connect.client_id = span(argv[7]);
  connect.username = span(argv[6]);
  flowie_mqtt_client_t *client = NULL;
  int reason = -1;
  int status = flowie_mqtt_client_create(&config, &client);
  if (status == SALTS_OK) status = flowie_mqtt_client_connect(client, &connect);
  if (status == SALTS_OK) status = wait_for_result(&state, &reason);
  if (client) {
    const int close_status = flowie_mqtt_client_try_destroy(client, 2500);
    if (close_status != SALTS_OK) {
      fprintf(stderr, "MQTT client did not drain (%d)\n", close_status);
      return EXIT_FAILURE;
    }
  }
  sync_cond_destroy(&state.ready);
  sync_mutex_destroy(&state.mutex);
  if (status != SALTS_OK) {
    printf("ERROR %d\n", status);
    return EXIT_FAILURE;
  }
  printf("PUBACK %d\n", reason);
  return reason < 128 ? EXIT_SUCCESS : EXIT_FAILURE;
}
