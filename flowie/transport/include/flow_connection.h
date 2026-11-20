#ifndef FLOW_CONNECTION_H
#define FLOW_CONNECTION_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Internal transport health; independent from any application runtime state model. */
typedef enum flowie_connection_state_e {
  FLOWIE_CONNECTION_STOPPED = 0,
  FLOWIE_CONNECTION_CONNECTING,
  FLOWIE_CONNECTION_READY,
  FLOWIE_CONNECTION_CLOSING,
  FLOWIE_CONNECTION_FAILED
} flowie_connection_state_t;

#define FLOWIE_ENDPOINT_MAX 510u

typedef struct flowie_connection_snapshot_s {
  size_t size;
  char endpoint[FLOWIE_ENDPOINT_MAX + 1u];
  flowie_connection_state_t state;
  int last_status;
  uint64_t connections_current;
  uint64_t connection_limit;
  uint64_t in_flight_messages;
  uint64_t in_flight_bytes;
} flowie_connection_snapshot_t;

typedef struct tf_connection_state_s {
  char endpoint[FLOWIE_ENDPOINT_MAX + 1u];
  atomic_int state;
  atomic_int last_status;
  atomic_uint_fast64_t connections_current;
  atomic_uint_fast64_t connection_limit;
  atomic_uint_fast64_t in_flight_messages;
  atomic_uint_fast64_t in_flight_bytes;
} tf_connection_state_t;

int tf_connection_init(tf_connection_state_t *connection, const char *endpoint,
                       uint64_t connection_limit);
int tf_connection_set_endpoint(tf_connection_state_t *connection, const char *endpoint);
void tf_connection_transition(tf_connection_state_t *connection,
                              flowie_connection_state_t state, int status);
void tf_connection_set_usage(tf_connection_state_t *connection, uint64_t connections_current,
                             uint64_t in_flight_messages, uint64_t in_flight_bytes);
int tf_connection_request_begin(tf_connection_state_t *connection, uint64_t bytes);
int tf_connection_request_end(tf_connection_state_t *connection, uint64_t bytes);
int tf_connection_snapshot(const tf_connection_state_t *connection,
                           flowie_connection_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif
