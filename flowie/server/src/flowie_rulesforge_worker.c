#include "flowie_rulesforge_worker_internal.h"

#include <core/clock.h>
#include <core/error.h>
#include <core/process.h>
#include <core/uuid.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

typedef HANDLE worker_pipe_t;
#define INVALID_WORKER_PIPE INVALID_HANDLE_VALUE
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int worker_pipe_t;
#define INVALID_WORKER_PIPE (-1)
#endif

struct flowie_rulesforge_worker_s {
  process_t *process;
  worker_pipe_t input;
  worker_pipe_t output;
  uint32_t timeout_ms;
  uint64_t last_batch;
  int unsettled;
  int drain_required;
  int fault;
  unsigned char frame[FLOWIE_RULESFORGE_FRAME_MAX];
};

static uint32_t worker_remaining(uint64_t deadline) {
  const uint64_t now = monotonic_ms();
  return deadline > now ? (uint32_t)(deadline - now) : 0;
}

#ifdef _WIN32
static void worker_pipe_close(worker_pipe_t pipe) {
  if (pipe != INVALID_WORKER_PIPE) CloseHandle(pipe);
}

static int worker_pipe_pair(const char *prefix, int sending, worker_pipe_t *parent,
                            worker_pipe_t *child) {
  char name[] = "\\\\.\\pipe\\flowie-rulesforge-00000000-0000-0000-0000-000000000000-i";
  const size_t offset = sizeof("\\\\.\\pipe\\flowie-rulesforge-") - 1u;
  memcpy(name + offset, prefix, SALTS_UUID_STRING_LENGTH);
  name[sizeof(name) - 2u] = sending ? 'i' : 'o';
  *parent = CreateNamedPipeA(name, (sending ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
      FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1u, 16384u, 16384u, 0u, NULL);
  if (*parent == INVALID_WORKER_PIPE) return SALTS_EIO;
  *child = CreateFileA(name, sending ? GENERIC_READ : GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  return *child == INVALID_WORKER_PIPE ? SALTS_EIO : SALTS_OK;
}

static int worker_spawn(flowie_rulesforge_worker_t *worker, const process_options_t *options) {
  unsigned char id[SALTS_UUID_SIZE];
  char name[SALTS_UUID_STRING_SIZE];
  int rc = uuid_v4_generate(id);
  if (rc == SALTS_OK) rc = uuid_format(id, name, sizeof(name));
  worker_pipe_t child_input = INVALID_WORKER_PIPE, child_output = INVALID_WORKER_PIPE;
  if (rc == SALTS_OK) rc = worker_pipe_pair(name, 1, &worker->input, &child_input);
  if (rc == SALTS_OK) rc = worker_pipe_pair(name, 0, &worker->output, &child_output);
  if (rc == SALTS_OK) {
    const process_stdio_bindings_t stdio = {
        (uintptr_t)child_input, (uintptr_t)child_output, SALTS_PROCESS_STDIO_INHERIT};
    rc = process_spawn_with_stdio(options, &stdio, &worker->process);
  }
  worker_pipe_close(child_input);
  worker_pipe_close(child_output);
  return rc;
}

static int worker_pipe_io(worker_pipe_t pipe, int sending, unsigned char *bytes, size_t size,
                          uint64_t deadline) {
  OVERLAPPED op = {0};
  op.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!op.hEvent) return SALTS_EIO;
  int rc = SALTS_OK;
  while (size) {
    DWORD count = 0;
    const DWORD remaining = worker_remaining(deadline);
    if (!remaining) {
      rc = SALTS_ETIMEDOUT;
      break;
    }
    ResetEvent(op.hEvent);
    const BOOL immediate = sending ? WriteFile(pipe, bytes, (DWORD)size, &count, &op)
                                  : ReadFile(pipe, bytes, (DWORD)size, &count, &op);
    if (!immediate) {
      DWORD error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(op.hEvent, remaining);
        if (wait != WAIT_OBJECT_0) {
          CancelIoEx(pipe, &op);
          (void)GetOverlappedResult(pipe, &op, &count, TRUE);
          rc = wait == WAIT_TIMEOUT ? SALTS_ETIMEDOUT : SALTS_EIO;
          break;
        }
        error = GetOverlappedResult(pipe, &op, &count, FALSE) ? ERROR_SUCCESS : GetLastError();
      }
      if (error != ERROR_SUCCESS) {
        rc = error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ? SALTS_EPIPE : SALTS_EIO;
        break;
      }
    }
    if (!count) {
      rc = SALTS_EPIPE;
      break;
    }
    bytes += count;
    size -= count;
  }
  CloseHandle(op.hEvent);
  return rc;
}

static void worker_end_input(flowie_rulesforge_worker_t *worker) {
  worker_pipe_close(worker->input);
  worker->input = INVALID_WORKER_PIPE;
}
#else
static int worker_errno(void) {
  return errno == EPIPE || errno == ECONNRESET ? SALTS_EPIPE :
         errno == EINTR ? SALTS_EINTR : SALTS_EIO;
}

static void worker_pipe_close(worker_pipe_t pipe) {
  if (pipe != INVALID_WORKER_PIPE) close(pipe);
}

static int worker_spawn(flowie_rulesforge_worker_t *worker, const process_options_t *options) {
  int pair[2];
#ifdef SOCK_CLOEXEC
  const int type = SOCK_STREAM | SOCK_CLOEXEC;
#else
  const int type = SOCK_STREAM;
#endif
  if (socketpair(AF_UNIX, type, 0, pair) != 0) return worker_errno();
  worker->input = worker->output = pair[0];
  int rc;
  if (fcntl(pair[0], F_SETFD, FD_CLOEXEC) == -1 ||
      fcntl(pair[1], F_SETFD, FD_CLOEXEC) == -1 ||
      fcntl(pair[0], F_SETFL, O_NONBLOCK) == -1) {
    rc = worker_errno();
  } else {
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
      rc = worker_errno();
    } else
#endif
    {
      const process_stdio_bindings_t stdio = {
          (uintptr_t)pair[1], (uintptr_t)pair[1], SALTS_PROCESS_STDIO_INHERIT};
      rc = process_spawn_with_stdio(options, &stdio, &worker->process);
    }
  }
  worker_pipe_close(pair[1]);
  return rc;
}

static int worker_pipe_io(worker_pipe_t pipe, int sending, unsigned char *bytes, size_t size,
                          uint64_t deadline) {
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  struct pollfd wait = {pipe, sending ? POLLOUT : POLLIN, 0};
  while (size) {
    const uint32_t remaining = worker_remaining(deadline);
    if (!remaining) return SALTS_ETIMEDOUT;
    int ready = poll(&wait, 1u, (int)remaining);
    if (ready == 0) return SALTS_ETIMEDOUT;
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) return worker_errno();
    ssize_t count = sending ? send(pipe, bytes, size, flags) : recv(pipe, bytes, size, 0);
    if (count == 0) return SALTS_EPIPE;
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count < 0) return worker_errno();
    bytes += (size_t)count;
    size -= (size_t)count;
  }
  return SALTS_OK;
}

static void worker_end_input(flowie_rulesforge_worker_t *worker) {
  (void)shutdown(worker->input, SHUT_WR);
  worker->input = INVALID_WORKER_PIPE;
}
#endif

static uint64_t worker_load(const unsigned char *bytes, size_t size) {
  uint64_t value = 0;
  for (size_t i = 0; i < size; ++i) value = (value << 8u) | bytes[i];
  return value;
}

static void worker_store(unsigned char *bytes, size_t size, uint64_t value) {
  while (size) {
    bytes[--size] = (unsigned char)value;
    value >>= 8u;
  }
}

static int worker_fail(flowie_rulesforge_worker_t *worker, int status) {
  worker->fault = status;
  return status;
}

static int worker_receive(flowie_rulesforge_worker_t *worker, uint64_t deadline, size_t *size) {
  unsigned char header[4];
  int rc = worker_pipe_io(worker->output, 0, header, sizeof(header), deadline);
  if (rc != SALTS_OK) return worker_fail(worker, rc);
  uint64_t length = worker_load(header, sizeof(header));
  if (!length || length > sizeof(worker->frame)) return worker_fail(worker, SALTS_EPROTO);
  *size = (size_t)length;
  rc = worker_pipe_io(worker->output, 0, worker->frame, *size, deadline);
  return rc == SALTS_OK ? rc : worker_fail(worker, rc);
}

static int worker_exchange(flowie_rulesforge_worker_t *worker, size_t request, size_t *response) {
  const uint64_t deadline = monotonic_ms() + worker->timeout_ms;
  unsigned char header[4];
  worker_store(header, sizeof(header), request);
  int rc = worker_pipe_io(worker->input, 1, header, sizeof(header), deadline);
  if (rc == SALTS_OK) rc = worker_pipe_io(worker->input, 1, worker->frame, request, deadline);
  return rc == SALTS_OK ? worker_receive(worker, deadline, response) : worker_fail(worker, rc);
}

static int worker_error_frame(const unsigned char *frame, size_t size) {
  if (size != 5u || frame[0] != 'E') return SALTS_EPROTO;
  switch (worker_load(frame + 1u, 4u)) {
  case 2: return SALTS_EINVAL;
  case 3: return SALTS_EPROTO;
  case 7: return SALTS_ENOMEM;
  case 10: return SALTS_ENOSPC;
  default: return SALTS_EIO;
  }
}

int flowie_rulesforge_worker_open(const char *program, const char *bundle,
    uint32_t timeout_ms, flowie_rulesforge_worker_t **out) {
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!program || !program[0] || !bundle || !bundle[0] || !timeout_ms || timeout_ms > 15000u)
    return SALTS_EINVAL;
  flowie_rulesforge_worker_t *worker = malloc(sizeof(*worker));
  if (!worker) return SALTS_ENOMEM;
  worker->process = NULL;
  worker->input = worker->output = INVALID_WORKER_PIPE;
  worker->timeout_ms = timeout_ms;
  worker->last_batch = 0;
  worker->unsettled = 0;
  worker->drain_required = 0;
  worker->fault = SALTS_OK;
  const char *args[] = {bundle, NULL};
  process_options_t options;
  process_options_init(&options);
  options.flags = 0;
  options.program = program;
  options.args = args;
  int rc = worker_spawn(worker, &options);
  if (rc == SALTS_OK) {
    size_t size = 0;
    rc = worker_receive(worker, monotonic_ms() + timeout_ms, &size);
    if (rc == SALTS_OK && (size != 1u || worker->frame[0] != 'R'))
      rc = worker_error_frame(worker->frame, size);
  }
  if (rc != SALTS_OK) {
    worker->fault = rc;
    (void)flowie_rulesforge_worker_close(&worker);
    return rc;
  }
  *out = worker;
  return SALTS_OK;
}

static int worker_read_field(const unsigned char **cursor, size_t *remaining,
    size_t maximum, const unsigned char **bytes, size_t *size) {
  if (*remaining < 2u) return SALTS_EPROTO;
  *size = (size_t)worker_load(*cursor, 2u);
  *cursor += 2u;
  *remaining -= 2u;
  if (*size > maximum || *size > *remaining) return SALTS_EPROTO;
  *bytes = *cursor;
  *cursor += *size;
  *remaining -= *size;
  return SALTS_OK;
}

static int worker_batch(flowie_rulesforge_worker_t *worker, size_t request,
                        flowie_rulesforge_batch_t *batch) {
  size_t size = 0;
  int rc = worker_exchange(worker, request, &size);
  if (rc != SALTS_OK) return rc;
  const unsigned char *frame = worker->frame;
  if (frame[0] == 'E') {
    rc = worker_error_frame(frame, size);
    return rc == SALTS_EINVAL || rc == SALTS_ENOSPC ? rc : worker_fail(worker, rc);
  }
  if (size < 20u || frame[0] != 'B' || frame[9] > 1u)
    return worker_fail(worker, SALTS_EPROTO);
  uint64_t id = worker_load(frame + 1u, 8u);
  size_t count = (size_t)worker_load(frame + 18u, 2u);
  if (!id || id != worker->last_batch + 1u || count > FLOWIE_RULESFORGE_OUTPUT_MAX)
    return worker_fail(worker, SALTS_EPROTO);
  const unsigned char *cursor = frame + 20u;
  size_t remaining = size - 20u;
  for (size_t i = 0; i < count; ++i) {
    const unsigned char *sink, *body;
    size_t sink_size, body_size;
    if (worker_read_field(&cursor, &remaining, FLOWIE_RULESFORGE_NAME_MAX, &sink, &sink_size) ||
        !sink_size || memchr(sink, 0, sink_size) ||
        worker_read_field(&cursor, &remaining, FLOWIE_RULESFORGE_BODY_MAX, &body, &body_size))
      return worker_fail(worker, SALTS_EPROTO);
    batch->outputs[i] = (flowie_rulesforge_output_t){(const char *)sink, sink_size, body, body_size};
  }
  if (remaining) return worker_fail(worker, SALTS_EPROTO);
  batch->id = id;
  batch->output_count = count;
  batch->expired = worker_load(frame + 10u, 8u);
  batch->drain_required = worker->drain_required = frame[9] != 0u;
  worker->last_batch = id;
  worker->unsettled = 1;
  return SALTS_OK;
}

static int worker_admit(flowie_rulesforge_worker_t *worker, flowie_rulesforge_batch_t *batch,
                        int draining) {
  if (!worker || !batch) return SALTS_EINVAL;
  batch->id = 0;
  batch->output_count = 0;
  batch->expired = 0;
  batch->drain_required = 0;
  if (worker->fault) return worker->fault;
  if (worker->unsettled || (worker->drain_required && !draining)) return SALTS_EBUSY;
  return SALTS_OK;
}

int flowie_rulesforge_worker_push(flowie_rulesforge_worker_t *worker, int64_t time_ms,
    const char *event_id, const char *entry_point, const char *fact_type,
    const void *json, size_t json_size, flowie_rulesforge_batch_t *batch) {
  int rc = worker_admit(worker, batch, 0);
  if (rc != SALTS_OK) return rc;
  const char *names[] = {event_id, entry_point, fact_type};
  size_t offset = 9u;
  if (!json || !json_size || json_size > sizeof(worker->frame) || memchr(json, 0, json_size))
    return SALTS_EINVAL;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    const size_t length = names[i] ? strlen(names[i]) : 0;
    if (!length || length > FLOWIE_RULESFORGE_NAME_MAX) return SALTS_EINVAL;
    worker_store(worker->frame + offset, 2u, length);
    offset += 2u;
    memcpy(worker->frame + offset, names[i], length);
    offset += length;
  }
  if (json_size > sizeof(worker->frame) - offset) return SALTS_ENOSPC;
  worker->frame[0] = 'P';
  worker_store(worker->frame + 1u, 8u, (uint64_t)time_ms);
  memcpy(worker->frame + offset, json, json_size);
  return worker_batch(worker, offset + json_size, batch);
}

int flowie_rulesforge_worker_watermark(flowie_rulesforge_worker_t *worker, int64_t time_ms,
    flowie_rulesforge_batch_t *batch) {
  int rc = worker_admit(worker, batch, 0);
  if (rc != SALTS_OK) return rc;
  worker->frame[0] = 'W';
  worker_store(worker->frame + 1u, 8u, (uint64_t)time_ms);
  return worker_batch(worker, 9u, batch);
}

int flowie_rulesforge_worker_drain(flowie_rulesforge_worker_t *worker,
    flowie_rulesforge_batch_t *batch) {
  int rc = worker_admit(worker, batch, 1);
  if (rc != SALTS_OK) return rc;
  worker->frame[0] = 'D';
  return worker_batch(worker, 1u, batch);
}

int flowie_rulesforge_worker_ack(flowie_rulesforge_worker_t *worker, uint64_t batch_id) {
  if (!worker) return SALTS_EINVAL;
  if (worker->fault) return worker->fault;
  if (!worker->unsettled || batch_id != worker->last_batch) return SALTS_EINVAL;
  worker->frame[0] = 'A';
  worker_store(worker->frame + 1u, 8u, batch_id);
  size_t size = 0;
  int rc = worker_exchange(worker, 9u, &size);
  if (rc != SALTS_OK) return rc;
  if (size != 1u || worker->frame[0] != 'K')
    return worker_fail(worker, worker_error_frame(worker->frame, size));
  worker->unsettled = 0;
  return SALTS_OK;
}

int flowie_rulesforge_worker_close(flowie_rulesforge_worker_t **owner) {
  if (!owner || !*owner) return SALTS_OK;
  flowie_rulesforge_worker_t *worker = *owner;
  *owner = NULL;
  int rc = worker->fault;
  if (worker->process && !rc) {
    if (worker->unsettled || worker->drain_required) {
      rc = SALTS_ECANCELED;
    } else {
      worker_end_input(worker);
      process_result_t result;
      rc = process_wait_for(worker->process, worker->timeout_ms, &result);
      if (rc == SALTS_OK && (result.state != SALTS_PROCESS_EXITED || result.exit_code))
        rc = SALTS_EIO;
    }
  }
  if (worker->process) process_destroy(worker->process);
  if (worker->output != worker->input) worker_pipe_close(worker->output);
  worker_pipe_close(worker->input);
  free(worker);
  return rc;
}
