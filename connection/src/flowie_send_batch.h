#ifndef FLOWIE_SEND_BATCH_H
#define FLOWIE_SEND_BATCH_H

#include <cnet/cnet.h>

enum {
  FLOWIE_SEND_SMALL_BYTES = 256u,
  FLOWIE_SEND_COALESCE_BYTES = 4096u
};

/* Private TCP/TLS lowering, after canonical slice and aggregate-size validation.
 * The caller owns one reference per slice. On success adjacent small ranges may
 * be replaced with slices of one immutable pooled buffer (at most 4 KiB of
 * copied payload). Unmerged ranges retain their backing identity. On error the
 * array and all its references remain unchanged. No queue or timer is added. */
int flowie_send_batch_coalesce(mem_slice_t *segments, size_t *segment_count);

#endif
