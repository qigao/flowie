#define TINYTEST_NO_MAIN
#include <tinytest.h>

#include "flowie_rulesforge_worker_internal.h"

#include <core/error.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static const char *test_program;
static const char event_json[] = "{\"amount\":15,\"total\":0,\"done\":false}";

static int peer_frame(const unsigned char *body, size_t size) {
  const unsigned char length[] = {0, 0, 0, (unsigned char)size};
  return fwrite(length, 1u, sizeof(length), stdout) == sizeof(length) && !fflush(stdout) &&
         fwrite(body, 1u, size, stdout) == size && !fflush(stdout);
}

static int peer_main(const char *mode) {
#ifdef _WIN32
  if (_setmode(_fileno(stdin), _O_BINARY) < 0 || _setmode(_fileno(stdout), _O_BINARY) < 0)
    return 1;
#endif
  const int silent = strcmp(mode, "--peer-not-ready") == 0;
  if (!silent && !peer_frame((const unsigned char *)"R", 1u)) return 1;
  if (silent || strcmp(mode, "--peer-stalled") == 0) {
#ifdef _WIN32
    Sleep(10000);
#else
    sleep(10);
#endif
    return 1;
  }
  unsigned char header[4], request[512];
  unsigned int id = 0;
  int unsettled = 0;
  for (;;) {
    size_t read = fread(header, 1u, sizeof(header), stdin);
    if (!read && feof(stdin)) return unsettled;
    if (read != sizeof(header)) return 1;
    size_t size = ((size_t)header[0] << 24u) | ((size_t)header[1] << 16u) |
                  ((size_t)header[2] << 8u) | header[3];
    if (!size || size > sizeof(request) || fread(request, 1u, size, stdin) != size) return 1;
    if (request[0] == 'A') {
      if (!unsettled || size != 9u || request[8] != id) return 1;
      if (!peer_frame((const unsigned char *)"K", 1u)) return 1;
      unsettled = 0;
      continue;
    }
    if (unsettled || (request[0] != 'P' && request[0] != 'W' && request[0] != 'D')) return 1;
    unsigned char batch[36] = {'B'};
    batch[8] = (unsigned char)++id;
    batch[17] = request[0] == 'W' ? 1u : 0u;
    size_t response = 20u;
    if (request[0] != 'D') {
      const int high = id == 2u;
      batch[19] = 1;
      batch[21] = 6;
      memcpy(batch + 22u, "alerts", 6u);
      batch[29] = high ? 4u : 6u;
      memcpy(batch + 30u, high ? "high" : "normal", batch[29]);
      response = 30u + batch[29];
    }
    if (strcmp(mode, "--peer-bad-batch") == 0) batch[8] = 99;
    if (!peer_frame(batch, response)) return 1;
    unsettled = 1;
  }
}

static void check_output(const flowie_rulesforge_batch_t *batch, const char *body) {
  check_equal(batch->output_count, 1u);
  check_equal(batch->outputs[0].sink_size, 6u);
  check_equal(memcmp(batch->outputs[0].sink, "alerts", 6u), 0);
  check_equal(batch->outputs[0].body_size, strlen(body));
  check_equal(memcmp(batch->outputs[0].body, body, strlen(body)), 0);
}

spec("Flowie RulesForge worker process") {
  static flowie_rulesforge_worker_t *worker;
  static char *large;

  after_each() {
    (void)flowie_rulesforge_worker_close(&worker);
    free(large);
    large = NULL;
  }

  it("keeps one session across events and confirms each borrowed result") {
    const char *program = getenv("FLOWIE_TEST_RULESFORGE_EXECUTABLE");
    const char *bundle = getenv("FLOWIE_TEST_RULESFORGE_BUNDLE");
    check_equal(!!program, !!bundle);
    check_equal(flowie_rulesforge_worker_open(program ? program : test_program,
        bundle ? bundle : "--peer-normal", 5000u, &worker), SALTS_OK);
    flowie_rulesforge_batch_t batch = {0};
    check_equal(flowie_rulesforge_worker_push(worker, 200, "later", "orders", "Order",
        event_json, sizeof(event_json) - 1u, &batch), SALTS_OK);
    check_equal(batch.id, 1u);
    check_output(&batch, "normal");
    check_equal(flowie_rulesforge_worker_ack(worker, 2u), SALTS_EINVAL);
    check_equal(flowie_rulesforge_worker_ack(worker, batch.id), SALTS_OK);
    check_equal(flowie_rulesforge_worker_push(worker, 100, "earlier", "orders", "Order",
        event_json, sizeof(event_json) - 1u, &batch), SALTS_OK);
    check_equal(batch.id, 2u);
    check_output(&batch, "high");
    check_equal(flowie_rulesforge_worker_ack(worker, batch.id), SALTS_OK);
    check_equal(flowie_rulesforge_worker_watermark(worker, 201, &batch), SALTS_OK);
    check_equal(batch.id, 3u);
    check_equal(batch.expired, 1u);
    check_output(&batch, "normal");
    check_equal(flowie_rulesforge_worker_ack(worker, batch.id), SALTS_OK);
    check_equal(flowie_rulesforge_worker_drain(worker, &batch), SALTS_OK);
    check_equal(batch.id, 4u);
    check_equal(batch.output_count, 0u);
    check_equal(flowie_rulesforge_worker_ack(worker, batch.id), SALTS_OK);
    check_equal(flowie_rulesforge_worker_close(&worker), SALTS_OK);
    check_null(worker);
  }

  it("terminates a process that never reports ready") {
    check_equal(flowie_rulesforge_worker_open(test_program, "--peer-not-ready", 300u,
        &worker), SALTS_ETIMEDOUT);
    check_null(worker);
  }

  it("times out blocked I/O and keeps the original fault") {
    check_equal(flowie_rulesforge_worker_open(test_program, "--peer-stalled", 1200u,
        &worker), SALTS_OK);
    const size_t size = 900u * 1024u;
    large = malloc(size);
    check_not_null(large);
    memset(large, 'a', size);
    flowie_rulesforge_batch_t batch = {0};
    check_equal(flowie_rulesforge_worker_push(worker, 200, "event", "orders", "Order",
        large, size, &batch), SALTS_ETIMEDOUT);
    check_equal(batch.id, 0u);
    check_equal(flowie_rulesforge_worker_watermark(worker, 201, &batch), SALTS_ETIMEDOUT);
    check_equal(flowie_rulesforge_worker_close(&worker), SALTS_ETIMEDOUT);
  }

  it("rejects an unrelated batch and aborts an unsettled close") {
    check_equal(flowie_rulesforge_worker_open(test_program, "--peer-bad-batch", 5000u,
        &worker), SALTS_OK);
    flowie_rulesforge_batch_t batch = {0};
    check_equal(flowie_rulesforge_worker_watermark(worker, 201, &batch), SALTS_EPROTO);
    check_equal(batch.id, 0u);
    check_equal(flowie_rulesforge_worker_ack(worker, 99u), SALTS_EPROTO);
    check_equal(flowie_rulesforge_worker_close(&worker), SALTS_EPROTO);
    check_equal(flowie_rulesforge_worker_open(test_program, "--peer-normal", 5000u,
        &worker), SALTS_OK);
    check_equal(flowie_rulesforge_worker_drain(worker, &batch), SALTS_OK);
    check_equal(flowie_rulesforge_worker_drain(worker, &batch), SALTS_EBUSY);
    check_equal(flowie_rulesforge_worker_close(&worker), SALTS_ECANCELED);
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && strncmp(argv[1], "--peer-", 7u) == 0) return peer_main(argv[1]);
  test_program = argv[0];
  return ttest_main__(argc, argv, TTEST_INVOKE_SPEC_ADAPTER__, TT_USE_COLOR != 0, TT_USE_TAP != 0,
                      TTEST_IS_ATTY__() != 0, TTEST_PRINT_TRACE_DEFAULT__);
}
