#include "flowie_cluster_state_machine_internal.h"

#include "turbo_error.h"

#include <string.h>

static const uint8_t FLOWIE_OWNER_MAGIC[4] = {'F', 'W', 'O', 'R'};
static const uint8_t FLOWIE_DATA_MAGIC[4] = {'T', 'R', 'D', 'S'};

int flowie_cluster_state_machine_apply_batch(
    void *ctx, const tr_raft_entry_t *entries, size_t entry_count) {
  flowie_cluster_state_machine_t *state =
      (flowie_cluster_state_machine_t *)ctx;
  size_t index;
  if (!state || !state->owners.directory || !state->apply_publish || !entries ||
      entry_count == 0u)
    return TURBO_EINVAL;
  for (index = 0u; index < entry_count; ++index) {
    const tr_raft_entry_t *entry = &entries[index];
    int rc;
    if (entry->data_length < sizeof(FLOWIE_OWNER_MAGIC)) return TURBO_EPROTO;
    if (memcmp(entry->data, FLOWIE_OWNER_MAGIC,
               sizeof(FLOWIE_OWNER_MAGIC)) == 0) {
      rc = flowie_cluster_owner_projection_apply_batch(&state->owners, entry,
                                                        1u);
    } else if (memcmp(entry->data, FLOWIE_DATA_MAGIC,
                      sizeof(FLOWIE_DATA_MAGIC)) == 0) {
      tr_raft_data_descriptor_t descriptor;
      rc = tr_raft_data_descriptor_decode(entry->data, entry->data_length,
                                          &descriptor);
      if (rc == TURBO_OK)
        rc = state->apply_publish(state->publish_ctx, entry->index, entry->term,
                                  entry->command_id, &descriptor);
    } else {
      rc = TURBO_EPROTO;
    }
    if (rc != TURBO_OK) return rc;
  }
  return TURBO_OK;
}
