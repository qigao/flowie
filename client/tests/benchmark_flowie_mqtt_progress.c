#include "flowie_mqtt_progress_support.h"
#include "flowie_mqtt_client.h"
#include "tinytest.h"
#include "tls_test_support.h"
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

typedef struct progress_sample {
  uint64_t planned, attempted, received;
} progress_sample;

typedef struct progress_probe {
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  int connected, error;
  size_t count;
  progress_sample samples[PROGRESS_SAMPLES];
} progress_probe;

static progress_probe probe;
static flowie_mqtt_client_t *client;
static pid_t peer_pid;
static int control_fd = -1;
static int probe_initialized;
static char ca_file[512];

static void progress_connect(flowie_mqtt_client_t *value, int status,
                             const flowie_mqtt_control_packet_view_t *response, void *user) {
  progress_probe *p = user;
  (void)value;
  if (status == SALTS_OK && (!response || response->type != FLOWIE_MQTT_PACKET_CONNACK ||
                             response->reason_code != 0)) status = SALTS_EPROTO;
  cmeta_mutex_lock(&p->mutex);
  p->error = status;
  p->connected = status == SALTS_OK;
  cmeta_cond_signal(&p->changed);
  cmeta_mutex_unlock(&p->mutex);
}

static void progress_error(flowie_mqtt_client_t *value, int status, void *user) {
  progress_probe *p = user;
  (void)value;
  cmeta_mutex_lock(&p->mutex);
  p->error = status;
  cmeta_cond_signal(&p->changed);
  cmeta_mutex_unlock(&p->mutex);
}

static int progress_message(flowie_mqtt_client_t *value,
                            const flowie_mqtt_publish_view_t *message, void *user) {
  progress_probe *p = user;
  uint64_t seq = UINT64_MAX, planned = 0, attempted = 0;
  int rc = SALTS_OK;
  (void)value;
  if (message->qos != 0 || message->payload.size != PROGRESS_PAYLOAD_BYTES ||
      message->topic.size != sizeof(PROGRESS_TOPIC) - 1 ||
      memcmp(message->topic.data, PROGRESS_TOPIC, sizeof(PROGRESS_TOPIC) - 1)) rc = SALTS_EPROTO;
  if (rc == SALTS_OK) {
    memcpy(&seq, message->payload.data, 8);
    memcpy(&planned, message->payload.data + 8, 8);
    memcpy(&attempted, message->payload.data + 16, 8);
    for (size_t i = 24; i < PROGRESS_PAYLOAD_BYTES; ++i)
      if (message->payload.data[i] != 0x5a) rc = SALTS_EPROTO;
  }
  cmeta_mutex_lock(&p->mutex);
  const uint64_t received = cmeta_hrtime();
  if (seq != p->count || p->count >= PROGRESS_SAMPLES ||
      attempted < planned || received < attempted) rc = SALTS_EPROTO;
  if (rc == SALTS_OK) p->samples[p->count++] = (progress_sample){planned, attempted, received};
  else p->error = rc;
  if (p->count == PROGRESS_SAMPLES || rc != SALTS_OK) cmeta_cond_signal(&p->changed);
  cmeta_mutex_unlock(&p->mutex);
  return rc;
}

static int progress_wait(int connected_only) {
  const uint64_t deadline = cmeta_monotonic_ms() + PROGRESS_TIMEOUT_MS;
  int rc = SALTS_OK;
  cmeta_mutex_lock(&probe.mutex);
  while (probe.error == SALTS_OK &&
         !(connected_only ? probe.connected : probe.count == PROGRESS_SAMPLES)) {
    const uint64_t now = cmeta_monotonic_ms();
    if (now >= deadline) { rc = SALTS_ETIMEDOUT; break; }
    (void)cmeta_cond_timedwait(&probe.changed, &probe.mutex, (deadline - now) * UINT64_C(1000000));
  }
  if (probe.error != SALTS_OK) rc = probe.error;
  cmeta_mutex_unlock(&probe.mutex);
  return rc;
}

static int progress_spawn(size_t mode) {
  int pair[2];
  posix_spawn_file_actions_t actions;
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) return SALTS_EIO;
  int rc = posix_spawn_file_actions_init(&actions);
  if (rc != 0) { close(pair[0]); close(pair[1]); return SALTS_EIO; }
  rc = posix_spawn_file_actions_adddup2(&actions, pair[1], STDIN_FILENO);
  if (rc == 0 && pair[0] != STDIN_FILENO)
    rc = posix_spawn_file_actions_addclose(&actions, pair[0]);
  char mode_text[] = {(char)('0' + mode), 0};
  char *args[] = {FLOWIE_PROGRESS_PEER_PATH, mode_text, NULL};
  if (rc == 0) rc = posix_spawn(&peer_pid, FLOWIE_PROGRESS_PEER_PATH, &actions, NULL, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pair[1]);
  if (rc != 0) { close(pair[0]); peer_pid = 0; return SALTS_EIO; }
  control_fd = pair[0];
  struct timeval timeout = {30, 0};
  if (setsockopt(control_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
      setsockopt(control_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) return SALTS_EIO;
  return SALTS_OK;
}

static int progress_cleanup(void) {
  int rc = SALTS_OK;
  if (client) {
    rc = flowie_mqtt_client_try_destroy(client, 5000);
    if (rc == SALTS_OK) client = NULL;
  }
  if (control_fd >= 0) { close(control_fd); control_fd = -1; }
  if (peer_pid > 0) {
    /* Failure teardown kills only this fixture's spawned child, then reaps it. */
    (void)kill(peer_pid, SIGKILL);
    while (waitpid(peer_pid, NULL, 0) < 0 && errno == EINTR) {}
    peer_pid = 0;
  }
  if (!client && probe_initialized) {
    cmeta_cond_destroy(&probe.changed);
    cmeta_mutex_destroy(&probe.mutex);
    probe_initialized = 0;
  }
  tls_test_remove_file(ca_file);
  ca_file[0] = 0;
  return rc;
}

static int progress_compare(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

static void progress_report(const char *transport, size_t repeat, const char *metric,
                             uint64_t *values) {
  /* Bounded O(N log N) reporting, outside the measured interval. */
  qsort(values, PROGRESS_SAMPLES, sizeof(*values), progress_compare);
  printf("METRIC transport=%s repeat=%zu metric=%s n=%d p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f\n",
         transport, repeat, metric, PROGRESS_SAMPLES,
         values[(PROGRESS_SAMPLES * 50 + 99) / 100 - 1] / 1000.0,
         values[(PROGRESS_SAMPLES * 95 + 99) / 100 - 1] / 1000.0,
         values[(PROGRESS_SAMPLES * 99 + 99) / 100 - 1] / 1000.0,
         values[PROGRESS_SAMPLES - 1] / 1000.0);
}

spec("MQTT connected idle CPU and independent incoming progress") {
  static const char *names[] = {"TCP", "TLS", "WS", "WSS"};
  static const flowie_mqtt_client_transport_t transports[] = {
      FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, FLOWIE_MQTT_CLIENT_TRANSPORT_TLS,
      FLOWIE_MQTT_CLIENT_TRANSPORT_WS, FLOWIE_MQTT_CLIENT_TRANSPORT_WSS};
  after_each() { check_equal(progress_cleanup(), SALTS_OK); }
  for (size_t mode = 0; mode < 4; ++mode) {
    for (size_t repeat = 0; repeat < PROGRESS_REPEATS; ++repeat) {
      it("measures %s independent arrivals repeat %zu", names[mode], repeat) {
        check_null(client);
        memset(&probe, 0, sizeof(probe));
        cmeta_mutex_init(&probe.mutex);
        cmeta_cond_init(&probe.changed);
        probe_initialized = 1;
        check_equal(progress_spawn(mode), SALTS_OK);
        progress_control peer = {0};
        check_equal(progress_read(control_fd, &peer), SALTS_OK);
        check_equal(peer.status, SALTS_OK);
        check_greater(peer.value, 0u);
        const uint64_t control_wall_start = cmeta_hrtime(), control_cpu_start = progress_cpu_ns();
        cmeta_sleep_ms(PROGRESS_CONTROL_MS);
        const uint64_t control_cpu_end = progress_cpu_ns(), control_wall = cmeta_hrtime() - control_wall_start;
        check_not_equal(control_cpu_start, UINT64_MAX);
        check_not_equal(control_cpu_end, UINT64_MAX);

        flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
        const flowie_mqtt_client_topic_handler_t handler = {
            {(const uint8_t *)PROGRESS_TOPIC, sizeof(PROGRESS_TOPIC) - 1}, progress_message};
        config.transport = transports[mode];
        config.host = "127.0.0.1";
        config.port = (int)peer.value;
        config.path = "/mqtt";
        config.timeout_ms = 30000;
        config.on_connect = progress_connect;
        config.on_error = progress_error;
        config.user_data = &probe;
        config.topic_handlers = (flowie_mqtt_client_topic_handler_map_t){&handler, 1};
        if (mode == 1 || mode == 3) {
          check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
          config.tls.ca_file = ca_file;
        }
        check_equal(flowie_mqtt_client_create(&config, &client), SALTS_OK);
        flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
        connect.version = FLOWIE_MQTT_VERSION_5;
        connect.clean_start = 1;
        connect.keep_alive = 0;
        connect.client_id = (flowie_mqtt_span_t){(const uint8_t *)"idle-progress", 13};
        check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);
        check_equal(progress_wait(1), SALTS_OK);
        cmeta_sleep_ms(100); /* Exclude handshake and startup work. */
        uint64_t idle_cpu_start = 0, idle_cpu_end = 0, idle_wall = 0;
        benchmark_batch("connected idle window (not throughput)", 1) {
          const uint64_t idle_wall_start = cmeta_hrtime();
          idle_cpu_start = progress_cpu_ns();
          cmeta_sleep_ms(PROGRESS_IDLE_MS);
          idle_cpu_end = progress_cpu_ns();
          idle_wall = cmeta_hrtime() - idle_wall_start;
        }
        check_not_equal(idle_cpu_start, UINT64_MAX);
        check_not_equal(idle_cpu_end, UINT64_MAX);
        check_equal(progress_wait(1), SALTS_OK);

        /* No MQTT operation or callback submits work after CONNECT. Only the
         * out-of-band IPC releases the independently scheduled source process. */
        const progress_control start = {.value = PROGRESS_SAMPLES};
        check_equal(progress_write(control_fd, &start), SALTS_OK);
        check_equal(progress_wait(0), SALTS_OK);
        check_equal(progress_read(control_fd, &peer), SALTS_OK);
        check_equal(peer.status, SALTS_OK);
        check_equal(flowie_mqtt_client_try_destroy(client, 5000), SALTS_OK);
        client = NULL;
        const progress_control stop = {0};
        check_equal(progress_write(control_fd, &stop), SALTS_OK);
        int child_status = 0;
        pid_t reaped;
        do { reaped = waitpid(peer_pid, &child_status, 0); } while (reaped < 0 && errno == EINTR);
        check_equal(reaped, peer_pid);
        peer_pid = 0;
        check_true(WIFEXITED(child_status));
        check_equal(WEXITSTATUS(child_status), 0);

        printf("METRIC transport=%s repeat=%zu metric=idle wall_ms=%.3f cpu_ms=%.3f cpu_pct=%.6f control_cpu_pct=%.6f retries=%llu\n",
               names[mode], repeat, idle_wall / 1e6, (idle_cpu_end - idle_cpu_start) / 1e6,
               100.0 * (idle_cpu_end - idle_cpu_start) / idle_wall,
               100.0 * (control_cpu_end - control_cpu_start) / control_wall,
               (unsigned long long)peer.retries);
        uint64_t release[PROGRESS_SAMPLES], attempted[PROGRESS_SAMPLES], planned[PROGRESS_SAMPLES];
        for (size_t i = 0; i < PROGRESS_SAMPLES; ++i) {
          const progress_sample *s = &probe.samples[i];
          release[i] = s->attempted - s->planned;
          attempted[i] = s->received - s->attempted;
          planned[i] = s->received - s->planned;
          printf("SAMPLE,%s,%zu,%zu,%llu,%llu,%llu\n", names[mode], repeat, i,
                 (unsigned long long)s->planned, (unsigned long long)s->attempted,
                 (unsigned long long)s->received);
        }
        progress_report(names[mode], repeat, "source_release", release);
        progress_report(names[mode], repeat, "attempt_to_receive", attempted);
        progress_report(names[mode], repeat, "planned_to_receive", planned);
      }
    }
  }
}
