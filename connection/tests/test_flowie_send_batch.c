#include "../src/flowie_send_batch.h"
#include <salts/error_codes.h>
#include "tinytest.h"
#include <string.h>

static mem_slice_t batch[CNET_RETAINED_VECTOR_MAX];
static size_t count;
static unsigned char expected[32768];
static size_t expected_size;

static void append_packet(size_t bytes, unsigned char value) {
  mem_buffer_t *buffer = mem_get_buffer(mem_global(), bytes + 7u);
  check_not_null(buffer);
  memset(mem_buffer_data(buffer), value, bytes + 7u);
  mem_set_used(buffer, bytes + 7u);
  batch[count++] = mem_slice(buffer, 7u, bytes);
  mem_buffer_release(buffer);
  memset(expected + expected_size, value, bytes);
  expected_size += bytes;
}

static void check_stream(void) {
  size_t offset = 0u;
  for (size_t i = 0u; i < count; ++i) {
    check_less_equal(batch[i].length, expected_size - offset);
    check_equal(batch[i].data, expected + offset, batch[i].length);
    offset += batch[i].length;
  }
  check_equal(offset, expected_size);
}

spec("Flowie bounded small-packet storage") {
  before_each() {
    count = 0u;
    expected_size = 0u;
    memset(batch, 0, sizeof(batch));
  }
  after_each() {
    for (size_t i = 0u; i < CNET_RETAINED_VECTOR_MAX; ++i)
      mem_slice_release(&batch[i]);
  }

  it("packs a full vector of tiny replies without changing bytes or order") {
    for (size_t i = 0u; i < CNET_RETAINED_VECTOR_MAX; ++i)
      append_packet(2u, (unsigned char)i);
    check_equal(flowie_send_batch_coalesce(batch, &count), SALTS_OK);
    check_equal(count, 1u);
    check_stream();
  }

  it("retains singleton and large payload backing identities") {
    mem_buffer_t *identities[3];
    append_packet(2u, 1u);
    append_packet(FLOWIE_SEND_SMALL_BYTES + 1u, 2u);
    append_packet(2u, 3u);
    for (size_t i = 0u; i < count; ++i) identities[i] = batch[i].buffer;
    check_equal(flowie_send_batch_coalesce(batch, &count), SALTS_OK);
    check_equal(count, 3u);
    for (size_t i = 0u; i < count; ++i) check_true(batch[i].buffer == identities[i]);
    check_stream();
  }

  it("packs small runs around an unchanged large payload into one bounded backing") {
    mem_buffer_t *large;
    append_packet(2u, 1u);
    append_packet(4u, 2u);
    append_packet(8192u, 3u);
    large = batch[2].buffer;
    append_packet(8u, 4u);
    append_packet(FLOWIE_SEND_SMALL_BYTES, 5u);
    check_equal(flowie_send_batch_coalesce(batch, &count), SALTS_OK);
    check_equal(count, 3u);
    check_true(batch[0].buffer == batch[2].buffer);
    check_true(batch[1].buffer == large);
    check_stream();
  }

  it("stops copying at exactly 4 KiB and preserves excess slices") {
    mem_buffer_t *last;
    for (size_t i = 0u; i < FLOWIE_SEND_COALESCE_BYTES / FLOWIE_SEND_SMALL_BYTES; ++i)
      append_packet(FLOWIE_SEND_SMALL_BYTES, (unsigned char)i);
    append_packet(1u, 99u);
    last = batch[count - 1u].buffer;
    check_equal(flowie_send_batch_coalesce(batch, &count), SALTS_OK);
    check_equal(count, 2u);
    check_equal(batch[0].length, (size_t)FLOWIE_SEND_COALESCE_BYTES);
    check_true(batch[1].buffer == last);
    check_stream();
  }

  it("does not copy a lone ready control reply") {
    append_packet(2u, 1u);
    mem_buffer_t *original = batch[0].buffer;
    check_equal(flowie_send_batch_coalesce(batch, &count), SALTS_OK);
    check_equal(count, 1u);
    check_true(batch[0].buffer == original);
    check_stream();
  }

  it("preserves caller references and makes merged bytes independent of their source") {
    append_packet(2u, 1u);
    append_packet(2u, 2u);
    mem_buffer_t *caller = mem_buffer_retain(batch[0].buffer);
    check_not_null(caller);
    const int rc = flowie_send_batch_coalesce(batch, &count);
    const uint32_t references = mem_buffer_ref_count(caller);
    /* The merged output no longer shares these bytes; the caller still owns
     * its source. This mutation must not affect the newly owned output. */
    if (rc == SALTS_OK) memset(mem_buffer_data(caller), 99, 9u);
    mem_buffer_release(caller);
    check_equal(rc, SALTS_OK);
    check_equal(references, 1u);
    check_equal(count, 1u);
    check_stream();
  }

  it("rejects invalid vector counts before changing caller ownership") {
    append_packet(2u, 1u);
    mem_buffer_t *original = batch[0].buffer;
    size_t invalid = CNET_RETAINED_VECTOR_MAX + 1u;
    check_equal(flowie_send_batch_coalesce(batch, &invalid), SALTS_EINVAL);
    invalid = 0u;
    check_equal(flowie_send_batch_coalesce(batch, &invalid), SALTS_EINVAL);
    check_equal(flowie_send_batch_coalesce(NULL, &count), SALTS_EINVAL);
    check_equal(flowie_send_batch_coalesce(batch, NULL), SALTS_EINVAL);
    check_true(batch[0].buffer == original);
  }
}
