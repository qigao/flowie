#include "../src/flowie_send_batch.h"
#include <salts/error_codes.h>
#include "tinytest.h"
#include <string.h>

/* Measures only owned-vector preparation/release, including the copy cost.
 * Network, TLS record processing and scheduling are deliberately not measured.
 * One operation is a complete 32-range batch, not an individual MQTT reply. */
enum { BATCH_SAMPLES = 10000u };
static mem_buffer_t *source[CNET_RETAINED_VECTOR_MAX];
static mem_slice_t owned[CNET_RETAINED_VECTOR_MAX];
static size_t lengths[CNET_RETAINED_VECTOR_MAX];
static size_t count;

static void prepare(void) {
  count = CNET_RETAINED_VECTOR_MAX;
  for (size_t i = 0u; i < count; ++i) owned[i] = mem_slice(source[i], 0u, lengths[i]);
}

static void release(void) {
  for (size_t i = 0u; i < count; ++i) mem_slice_release(&owned[i]);
  count = 0u;
}

spec("Flowie send vector preparation cost") {
  static const size_t sizes[] = {2u, 64u, 256u, 8192u, 0u};
  after_each() {
    release();
    for (size_t i = 0u; i < CNET_RETAINED_VECTOR_MAX; ++i) {
      mem_buffer_release(source[i]);
      source[i] = NULL;
    }
  }
  for (size_t scenario = 0u; scenario < sizeof(sizes) / sizeof(sizes[0]); ++scenario) {
    it("prepares 32 ranges of %zu bytes (zero means mixed)", sizes[scenario]) {
      for (size_t i = 0u; i < CNET_RETAINED_VECTOR_MAX; ++i) {
        lengths[i] = sizes[scenario] ? sizes[scenario] : (i % 3u == 2u ? 8192u : 64u);
        source[i] = mem_get_buffer(mem_global(), lengths[i]);
        check_not_null(source[i]);
        memset(mem_buffer_data(source[i]), (int)i, lengths[i]);
        mem_set_used(source[i], lengths[i]);
      }
      prepare();
      check_equal(flowie_send_batch_coalesce(owned, &count), SALTS_OK);
      check_equal(count, sizes[scenario] == 2u || sizes[scenario] == 64u ? 1u :
                         sizes[scenario] == 256u ? 17u : sizes[scenario] == 8192u ? 32u : 21u);
      release();

      benchmark_batch("retained vector prepare/release", BATCH_SAMPLES) {
        prepare();
        release();
      }
      int status = SALTS_OK;
      benchmark_batch("small-packet vector prepare/release", BATCH_SAMPLES) {
        prepare();
        const int rc = flowie_send_batch_coalesce(owned, &count);
        if (rc != SALTS_OK) status = rc;
        release();
      }
      check_equal(status, SALTS_OK);
    }
  }
}
