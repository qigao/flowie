#ifndef FLOWIE_MQTT_PROGRESS_SUPPORT_H
#define FLOWIE_MQTT_PROGRESS_SUPPORT_H

#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/thread.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
  PROGRESS_SAMPLES = 512,
  PROGRESS_REPEATS = 3,
  PROGRESS_PAYLOAD_BYTES = 64,
  PROGRESS_IDLE_MS = 5000,
  PROGRESS_CONTROL_MS = 1000,
  PROGRESS_TIMEOUT_MS = 15000
};
#define PROGRESS_INTERVAL_NS UINT64_C(7000000)
#define PROGRESS_TOPIC "bench/inbound"

/* Test-local IPC between exec-separated peers on the same host/ABI. The
 * producer owns its packet until copied send admission succeeds. No pointers
 * cross processes. Each receiver owns a fixed 512-entry result array; its
 * callback publishes completion before the controller reads the array.
 * Full admission retries preserve the first attempt timestamp, with a bounded
 * deadline. Stop/join completes before fixture or callback storage is freed. */
typedef struct progress_control {
  uint64_t value;
  uint64_t retries;
  int status;
} progress_control;

static inline int progress_write(int fd, const progress_control *value) {
  const unsigned char *p = (const unsigned char *)value;
  size_t left = sizeof(*value);
  while (left) {
    ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return SALTS_EIO;
    p += n;
    left -= (size_t)n;
  }
  return SALTS_OK;
}

static inline int progress_read(int fd, progress_control *value) {
  unsigned char *p = (unsigned char *)value;
  size_t left = sizeof(*value);
  while (left) {
    ssize_t n = recv(fd, p, left, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return SALTS_EIO;
    p += n;
    left -= (size_t)n;
  }
  return SALTS_OK;
}

/* Sleep to an absolute schedule without changing the next planned release
 * when the VM deschedules the source. Late messages consequently catch up. */
static inline int progress_sleep_until(uint64_t planned) {
  for (;;) {
    const uint64_t now = cmeta_hrtime();
    if (now >= planned) return SALTS_OK;
    const uint64_t remaining = planned - now;
    const struct timespec delay = {(time_t)(remaining / UINT64_C(1000000000)),
                                  (long)(remaining % UINT64_C(1000000000))};
    if (nanosleep(&delay, NULL) != 0 && errno != EINTR) return SALTS_EIO;
  }
}

static inline uint64_t progress_cpu_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) return UINT64_MAX;
  return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

#endif
