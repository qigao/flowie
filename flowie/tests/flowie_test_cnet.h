#ifndef FLOWIE_TEST_CNET_H
#define FLOWIE_TEST_CNET_H

#include <cnet/cnet.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_TEST_CNET_TIMEOUT_MS = 5000u,
  FLOWIE_TEST_CNET_BUFFER_CAPACITY = 128u * 1024u,
  FLOWIE_TEST_CNET_CLIENT_CAPACITY = 2u,
  FLOWIE_TEST_CNET_COMMAND_CAPACITY = 16u,
  FLOWIE_TEST_CNET_REQUEST_CAPACITY = 8u,
  FLOWIE_TEST_CNET_EVENT_CAPACITY = 16u,
  FLOWIE_TEST_CNET_MAX_SEND_BYTES = 128u * 1024u
};

typedef struct flowie_test_cnet_client_s {
  cnet_client network;
  cnet_connection connection;
  int connected;
  int closed;
  int failed;
  int sent;
  int status;
  unsigned char received[FLOWIE_TEST_CNET_BUFFER_CAPACITY];
  size_t received_size;
} flowie_test_cnet_client_t;

#define FLOWIE_TEST_INVALID_CNET_CLIENT NULL

static native_io_backend_kind flowie_test_cnet_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config flowie_test_cnet_config(void) {
  return (cnet_client_config){.backend = flowie_test_cnet_backend(),
                              .connection_capacity = FLOWIE_TEST_CNET_CLIENT_CAPACITY,
                              .command_capacity = FLOWIE_TEST_CNET_COMMAND_CAPACITY,
                              .request_capacity = FLOWIE_TEST_CNET_REQUEST_CAPACITY,
                              .completion_batch_capacity = FLOWIE_TEST_CNET_REQUEST_CAPACITY,
                              .event_capacity = FLOWIE_TEST_CNET_EVENT_CAPACITY,
                              .max_send_bytes = FLOWIE_TEST_CNET_MAX_SEND_BYTES,
                              .receive_buffer_bytes = FLOWIE_TEST_CNET_BUFFER_CAPACITY};
}

static void flowie_test_cnet_state(void *user, cnet_connection connection,
                                   cnet_connection_state state, const cnet_error *error) {
  flowie_test_cnet_client_t *client = (flowie_test_cnet_client_t *)user;
  (void)connection;
  if (!client) return;
  if (state == CNET_CONNECTION_CONNECTED) {
    client->connected = 1;
    client->status = SALTS_OK;
  } else if (state == CNET_CONNECTION_FAILED) {
    client->failed = 1;
    client->status = error ? error->status : SALTS_EIO;
  } else if (state == CNET_CONNECTION_CLOSING || state == CNET_CONNECTION_CLOSED) {
    client->closed = 1;
    if (client->status == SALTS_OK && error) client->status = error->status;
  }
}

static void flowie_test_cnet_receive(void *user, cnet_connection connection,
                                     const cnet_receive_view *view) {
  flowie_test_cnet_client_t *client = (flowie_test_cnet_client_t *)user;
  size_t available;
  (void)connection;
  if (!client || !view || !view->data || view->kind != CNET_MESSAGE_BYTES) return;
  available = sizeof(client->received) - client->received_size;
  if (view->size > available) {
    client->failed = 1;
    client->status = SALTS_EMSGSIZE;
    return;
  }
  memcpy(client->received + client->received_size, view->data, view->size);
  client->received_size += view->size;
  if (client->connected && client->received_size < sizeof(client->received))
    (void)cnet_receive(&client->network, client->connection,
                       sizeof(client->received) - client->received_size);
}

static void flowie_test_cnet_on_send(void *user, cnet_connection connection, size_t size) {
  flowie_test_cnet_client_t *client = (flowie_test_cnet_client_t *)user;
  (void)connection;
  (void)size;
  if (client) client->sent = 1;
}

static int flowie_test_cnet_poll(flowie_test_cnet_client_t *client, uint32_t timeout_ms) {
  size_t events = 0u;
  int status;
  if (!client) return SALTS_EINVAL;
  status = cnet_client_poll(&client->network, timeout_ms, &events);
  if (status != SALTS_OK) {
    client->failed = 1;
    client->status = status;
    return status;
  }
  if (client->failed) return client->status == SALTS_OK ? SALTS_EIO : client->status;
  return SALTS_OK;
}

static int flowie_test_cnet_wait(flowie_test_cnet_client_t *client, int *condition,
                                 uint32_t timeout_ms) {
  const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
  int status;
  if (!client || !condition) return SALTS_EINVAL;
  while (!*condition) {
    const uint64_t now = salts_monotonic_ms();
    uint32_t slice;
    if (now >= deadline) return SALTS_ETIMEDOUT;
    slice = (uint32_t)(deadline - now);
    if (slice > 50u) slice = 50u;
    status = flowie_test_cnet_poll(client, slice);
    if (status != SALTS_OK) return status;
  }
  return SALTS_OK;
}

static unsigned short flowie_test_cnet_port(void) {
  cnet_listener listener = {0};
  cnet_listener_config config = {.backend = flowie_test_cnet_backend(),
                                  .host = "127.0.0.1",
                                  .port = 0u,
                                  .backlog = 1u};
  uint16_t port = 0u;
  int status = cnet_listener_init(&listener, &config);
  if (status == SALTS_OK) status = cnet_listener_port(&listener, &port);
  if (listener.impl) {
    (void)cnet_listener_close(&listener);
    (void)cnet_listener_destroy(&listener);
  }
  return status == SALTS_OK ? port : 0u;
}

static flowie_test_cnet_client_t *flowie_test_cnet_connect_with_recv_buffer(
    unsigned short port, size_t recv_buffer_bytes) {
  flowie_test_cnet_client_t *client;
  cnet_client_config config;
  cnet_connect_options options;
  char uri[64];
  int status;
  if (port == 0u || (recv_buffer_bytes != 0u && recv_buffer_bytes > INT_MAX)) return NULL;
  client = (flowie_test_cnet_client_t *)calloc(1u, sizeof(*client));
  if (!client) return NULL;
  client->status = SALTS_OK;
  config = flowie_test_cnet_config();
  status = cnet_client_init(&client->network, &config);
  if (status != SALTS_OK) goto fail;
  if (recv_buffer_bytes != 0u) {
    cnet_stream_socket_options socket_options = CNET_STREAM_SOCKET_OPTIONS_INIT;
    socket_options.receive_buffer_bytes = recv_buffer_bytes;
    status = cnet_client_set_stream_socket_options(&client->network, &socket_options);
    if (status != SALTS_OK) goto fail;
  }
  if (snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port) < 0) goto fail;
  options = (cnet_connect_options){.uri = uri,
                                   .observer = {.on_state = flowie_test_cnet_state,
                                                .on_receive = flowie_test_cnet_receive,
                                                .on_send = flowie_test_cnet_on_send,
                                                .user = client}};
  status = cnet_connect(&client->network, &options, &client->connection);
  if (status != SALTS_OK) goto fail;
  status = flowie_test_cnet_wait(client, &client->connected, FLOWIE_TEST_CNET_TIMEOUT_MS);
  if (status != SALTS_OK) goto fail;
  status = cnet_receive(&client->network, client->connection,
                        sizeof(client->received) - client->received_size);
  if (status != SALTS_OK) goto fail;
  return client;

fail:
  if (client->network.impl) {
    (void)cnet_client_stop(&client->network, FLOWIE_TEST_CNET_TIMEOUT_MS);
    (void)cnet_client_destroy(&client->network);
  }
  free(client);
  return NULL;
}

static flowie_test_cnet_client_t *flowie_test_cnet_connect(unsigned short port) {
  return flowie_test_cnet_connect_with_recv_buffer(port, 0u);
}

static int flowie_test_cnet_send(flowie_test_cnet_client_t *client, const uint8_t *data,
                                 size_t size) {
  int status;
  if (!client || !data || size == 0u || !client->connected || client->closed)
    return SALTS_EINVAL;
  client->sent = 0;
  status = cnet_send(&client->network, client->connection, data, size);
  if (status != SALTS_OK) return status;
  (void)flowie_test_cnet_poll(client, 0u);
  return SALTS_OK;
}

static int flowie_test_cnet_recv_exact(flowie_test_cnet_client_t *client, uint8_t *data,
                                       size_t size) {
  const uint64_t deadline = salts_monotonic_ms() + FLOWIE_TEST_CNET_TIMEOUT_MS;
  if (!client || (!data && size != 0u)) return SALTS_EINVAL;
  while (client->received_size < size && !client->closed && !client->failed &&
         salts_monotonic_ms() < deadline) {
    int status = flowie_test_cnet_poll(client, 50u);
    if (status != SALTS_OK) return status;
  }
  if (client->received_size < size)
    return client->failed ? client->status : SALTS_ETIMEDOUT;
  if (size != 0u) memcpy(data, client->received, size);
  memmove(client->received, client->received + size, client->received_size - size);
  client->received_size -= size;
  return SALTS_OK;
}

static int flowie_test_cnet_readable(flowie_test_cnet_client_t *client, uint32_t timeout_ms) {
  const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
  if (!client) return 0;
  if (client->received_size != 0u || client->closed || client->failed) return 1;
  while (client->received_size == 0u) {
    const uint64_t now = salts_monotonic_ms();
    uint32_t slice;
    if (now >= deadline) break;
    slice = (uint32_t)(deadline - now);
    if (slice > 50u) slice = 50u;
    if (flowie_test_cnet_poll(client, slice) != SALTS_OK) return 1;
  }
  return client->received_size != 0u;
}

static int flowie_test_cnet_recv_mqtt5_connack(flowie_test_cnet_client_t *client,
                                               uint8_t session_present,
                                               uint16_t receive_maximum,
                                               uint32_t maximum_packet_size) {
  uint8_t expected[] = {0x20u, 0x0bu, 0x00u, 0x00u, 0x08u, 0x21u, 0x00u,
                        0x00u, 0x27u, 0x00u, 0x00u, 0x00u, 0x00u};
  uint8_t received[sizeof(expected)];
  expected[2] = session_present;
  expected[6] = (uint8_t)(receive_maximum >> 8u);
  expected[7] = (uint8_t)receive_maximum;
  expected[9] = (uint8_t)(maximum_packet_size >> 24u);
  expected[10] = (uint8_t)(maximum_packet_size >> 16u);
  expected[11] = (uint8_t)(maximum_packet_size >> 8u);
  expected[12] = (uint8_t)maximum_packet_size;
  if (flowie_test_cnet_recv_exact(client, received, sizeof(received)) != SALTS_OK)
    return SALTS_EPROTO;
  return memcmp(received, expected, sizeof(expected)) == 0 ? SALTS_OK : SALTS_EPROTO;
}

static void flowie_test_cnet_close(flowie_test_cnet_client_t *client) {
  if (!client) return;
  if (client->network.impl) {
    (void)cnet_client_stop(&client->network, FLOWIE_TEST_CNET_TIMEOUT_MS);
    (void)cnet_client_destroy(&client->network);
  }
  free(client);
}

#endif
