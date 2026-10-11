#include "flowie_send_batch.h"

#include <salts/error_codes.h>
#include <string.h>

int flowie_send_batch_coalesce(mem_slice_t *segments, size_t *segment_count) {
  size_t runs[CNET_RETAINED_VECTOR_MAX] = {0};
  mem_slice_t output[CNET_RETAINED_VECTOR_MAX] = {0};
  size_t packed_bytes = 0u;
  size_t offset = 0u;
  size_t output_count = 0u;
  mem_buffer_t *packed;
  if (segments == NULL || segment_count == NULL || *segment_count == 0u ||
      *segment_count > CNET_RETAINED_VECTOR_MAX)
    return SALTS_EINVAL;

  /* O(n + copied bytes), fixed O(CNET_RETAINED_VECTOR_MAX) metadata. A singleton
   * is never copied. Once the copy budget is spent the remaining ranges retain
   * their existing ownership; a large range never consumes the copy budget. */
  for (size_t i = 0u; i < *segment_count;) {
    size_t end = i;
    size_t bytes = 0u;
    while (end < *segment_count && segments[end].length <= FLOWIE_SEND_SMALL_BYTES &&
           segments[end].length <= FLOWIE_SEND_COALESCE_BYTES - packed_bytes - bytes) {
      bytes += segments[end].length;
      ++end;
    }
    if (end - i < 2u) {
      ++i;
      continue;
    }
    runs[i] = end - i;
    packed_bytes += bytes;
    i = end;
  }
  if (packed_bytes == 0u) return SALTS_OK;
  packed = mem_get_buffer(mem_global(), packed_bytes);
  if (packed == NULL) return SALTS_ENOMEM;
  mem_set_used(packed, packed_bytes);

  for (size_t i = 0u; i < *segment_count;) {
    if (runs[i] == 0u) {
      output[output_count++] = segments[i++]; /* move only at commit below */
    } else {
      const size_t begin = offset;
      const size_t end = i + runs[i];
      for (size_t j = i; j < end; ++j) {
        memcpy((unsigned char *)mem_buffer_data(packed) + offset,
               segments[j].data, segments[j].length);
        offset += segments[j].length;
      }
      output[output_count] = mem_slice(packed, begin, offset - begin);
      if (output[output_count].buffer == NULL) {
        for (size_t j = 0u; j < output_count; ++j)
          if (output[j].buffer == packed) mem_slice_release(&output[j]);
        mem_buffer_release(packed);
        return SALTS_EPROTO;
      }
      ++output_count;
      i = end;
    }
  }
  /* Commit only after the replacement payload and references are complete. */
  for (size_t i = 0u; i < *segment_count;) {
    const size_t run = runs[i];
    if (run == 0u) {
      ++i;
    } else {
      for (size_t j = i; j < i + run; ++j) mem_slice_release(&segments[j]);
      i += run;
    }
  }
  memcpy(segments, output, output_count * sizeof(*segments));
  memset(segments + output_count, 0, (*segment_count - output_count) * sizeof(*segments));
  *segment_count = output_count;
  mem_buffer_release(packed);
  return SALTS_OK;
}
