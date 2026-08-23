#include "flowie_stl_error_internal.h"

#include <turbostl/deque.h>
#include <turbostl/hash_map.h>
#include <turbostl/hash_set.h>
#include <turbostl/vec.h>

#include "flowie_cluster_owner_directory_internal.h"

#include "turbo_error.h"
#include "turbo_str.h"
#include "turbo_thread.h"
#include <turbostl/vec.h>

#include <stdlib.h>
#include <string.h>

struct flowie_cluster_owner_directory_s {
  uint32_t hash_version;
  uint32_t shard_count;
  tstr cluster_id;
  tstr listener_id;
  vec_t active;
  vec_t scratch;
  uint64_t revision;
  uint32_t last_revision_shard;
  turbo_mutex_t mutex;
  int mutex_initialized;
  int active_initialized;
  int scratch_initialized;
};

static int flowie_cluster_owner_directory_boot_nonzero(
    const uint8_t boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE]) {
  uint8_t combined = 0u;
  size_t index;
  if (!boot_id) return 0;
  for (index = 0u; index < FLOWIE_CLUSTER_BOOT_ID_SIZE; ++index)
    combined |= boot_id[index];
  return combined != 0u;
}

static int flowie_cluster_owner_directory_boot_zero(
    const uint8_t boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE]) {
  uint8_t combined = 0u;
  size_t index;
  if (!boot_id) return 0;
  for (index = 0u; index < FLOWIE_CLUSTER_BOOT_ID_SIZE; ++index)
    combined |= boot_id[index];
  return combined == 0u;
}

static int flowie_cluster_owner_directory_token_valid(
    const flowie_cluster_owner_token_t *owner, uint32_t shard_id) {
  return owner && owner->size >= sizeof(*owner) &&
         owner->abi_version == FLOWIE_CLUSTER_INTERNAL_ABI_V1 && owner->shard_id == shard_id &&
         owner->owner_epoch != 0u && owner->node_id_size != 0u &&
         owner->node_id_size <= FLOWIE_CLUSTER_NODE_ID_MAX &&
         owner->node_id[owner->node_id_size] == '\0' &&
         flowie_cluster_owner_directory_boot_nonzero(owner->boot_id);
}

static int flowie_cluster_owner_directory_entry_valid(
    const flowie_cluster_owner_directory_entry_t *entry, uint32_t shard_id) {
  if (!entry || entry->size != sizeof(*entry) ||
      entry->abi_version != FLOWIE_CLUSTER_OWNER_DIRECTORY_ABI_V1 ||
      entry->shard_id != shard_id)
    return 0;
  if (entry->local_deadline_ns == 0u)
    return entry->owner.size >= sizeof(entry->owner) &&
           entry->owner.abi_version == FLOWIE_CLUSTER_INTERNAL_ABI_V1 &&
           entry->owner.shard_id == shard_id && entry->owner.owner_epoch == 0u &&
           entry->owner.node_id_size == 0u && entry->owner.node_id[0] == '\0' &&
           flowie_cluster_owner_directory_boot_zero(entry->owner.boot_id);
  return flowie_cluster_owner_directory_token_valid(&entry->owner, shard_id);
}

static int flowie_cluster_owner_directory_owner_equal(
    const flowie_cluster_owner_token_t *left, const flowie_cluster_owner_token_t *right) {
  return left->size == right->size && left->abi_version == right->abi_version &&
         left->shard_id == right->shard_id && left->owner_epoch == right->owner_epoch &&
         left->node_id_size == right->node_id_size &&
         left->node_id_size <= FLOWIE_CLUSTER_NODE_ID_MAX &&
         memcmp(left->node_id, right->node_id, left->node_id_size) == 0 &&
         memcmp(left->boot_id, right->boot_id, FLOWIE_CLUSTER_BOOT_ID_SIZE) == 0;
}

static int flowie_cluster_owner_directory_entries_equal(
    const vec_t *entries, const flowie_cluster_owner_directory_entry_t *incoming,
    size_t count) {
  size_t index;
  if (vec_size(entries) != count) return 0;
  for (index = 0u; index < count; ++index) {
    const flowie_cluster_owner_directory_entry_t *current =
        (const flowie_cluster_owner_directory_entry_t *)vec_at_const(entries, index);
    if (current->shard_id != incoming[index].shard_id ||
        current->local_deadline_ns != incoming[index].local_deadline_ns ||
        !flowie_cluster_owner_directory_owner_equal(&current->owner, &incoming[index].owner))
      return 0;
  }
  return 1;
}

static void flowie_cluster_owner_directory_free(flowie_cluster_owner_directory_t *directory) {
  if (!directory) return;
  if (directory->scratch_initialized) vec_destroy(&directory->scratch);
  if (directory->active_initialized) vec_destroy(&directory->active);
  if (directory->mutex_initialized) turbo_mutex_destroy(&directory->mutex);
  tstr_freep(&directory->listener_id);
  tstr_freep(&directory->cluster_id);
  free(directory);
}

int flowie_cluster_owner_directory_create(const flowie_cluster_owner_directory_config_t *config,
                                          flowie_cluster_owner_directory_t **out) {
  flowie_cluster_owner_directory_t *directory;
  int rc;
  if (out) *out = NULL;
  if (!config || config->size != sizeof(*config) ||
      config->abi_version != FLOWIE_CLUSTER_OWNER_DIRECTORY_ABI_V1 || !out ||
      config->hash_version != FLOWIE_CLUSTER_HASH_VERSION_1 || config->shard_count == 0u ||
      config->shard_count > FLOWIE_CLUSTER_SHARD_COUNT_MAX || !config->cluster_id.data ||
      config->cluster_id.len == 0u || config->cluster_id.len > FLOWIE_CLUSTER_ID_MAX ||
      memchr(config->cluster_id.data, '\0', config->cluster_id.len) ||
      !config->listener_id.data || config->listener_id.len == 0u ||
      config->listener_id.len > FLOWIE_CLUSTER_LISTENER_ID_MAX ||
      memchr(config->listener_id.data, '\0', config->listener_id.len))
    return TURBO_EINVAL;
  directory = (flowie_cluster_owner_directory_t *)calloc(1u, sizeof(*directory));
  if (!directory) return TURBO_ENOMEM;
  directory->hash_version = config->hash_version;
  directory->shard_count = config->shard_count;
  directory->cluster_id = tstr_from_v(config->cluster_id);
  directory->listener_id = tstr_from_v(config->listener_id);
  if (!directory->cluster_id || !directory->listener_id) {
    rc = TURBO_ENOMEM;
    goto fail;
  }
  turbo_mutex_init(&directory->mutex);
  directory->mutex_initialized = 1;
  rc = flowie_stl_error(vec_init_bytes(&directory->active, sizeof(flowie_cluster_owner_directory_entry_t), _Alignof(flowie_cluster_owner_directory_entry_t), SIZE_MAX));
  if (rc != TURBO_OK) goto fail;
  directory->active_initialized = 1;
  rc = flowie_stl_error(vec_init_bytes(&directory->scratch, sizeof(flowie_cluster_owner_directory_entry_t), _Alignof(flowie_cluster_owner_directory_entry_t), SIZE_MAX));
  if (rc != TURBO_OK) goto fail;
  directory->scratch_initialized = 1;
  rc = flowie_stl_error(vec_reserve(&directory->active, config->shard_count));
  if (rc == TURBO_OK) rc = flowie_stl_error(vec_reserve(&directory->scratch, config->shard_count));
  if (rc != TURBO_OK) goto fail;
  for (uint32_t shard_id = 0u; shard_id < config->shard_count; ++shard_id) {
    flowie_cluster_owner_directory_entry_t entry =
        FLOWIE_CLUSTER_OWNER_DIRECTORY_ENTRY_INIT;
    entry.shard_id = shard_id;
    entry.owner.shard_id = shard_id;
    rc = flowie_stl_error(vec_push(&directory->active, &entry));
    if (rc != TURBO_OK) goto fail;
  }
  directory->last_revision_shard = UINT32_MAX;
  *out = directory;
  return TURBO_OK;

fail:
  flowie_cluster_owner_directory_free(directory);
  return rc;
}

int flowie_cluster_owner_directory_replace(
    flowie_cluster_owner_directory_t *directory,
    const flowie_cluster_owner_directory_entry_t *entries, size_t entry_count,
    uint64_t revision) {
  size_t index;
  int rc;
  if (!directory || !entries || entry_count != directory->shard_count) return TURBO_EINVAL;
  turbo_mutex_lock(&directory->mutex);
  if (revision < directory->revision) {
    turbo_mutex_unlock(&directory->mutex);
    return TURBO_EBUSY;
  }
  if (revision == directory->revision && vec_size(&directory->active) != 0u) {
    rc = flowie_cluster_owner_directory_entries_equal(&directory->active, entries, entry_count)
             ? TURBO_OK
             : TURBO_EPROTO;
    turbo_mutex_unlock(&directory->mutex);
    return rc;
  }
  turbo_mutex_unlock(&directory->mutex);
  vec_clear(&directory->scratch);
  for (index = 0u; index < entry_count; ++index) {
    if (!flowie_cluster_owner_directory_entry_valid(&entries[index], (uint32_t)index))
      return TURBO_EINVAL;
    rc = flowie_stl_error(vec_push(&directory->scratch, &entries[index]));
    if (rc != TURBO_OK) return rc;
  }
  turbo_mutex_lock(&directory->mutex);
  if (revision < directory->revision) {
    turbo_mutex_unlock(&directory->mutex);
    vec_clear(&directory->scratch);
    return TURBO_EBUSY;
  }
  if (revision == directory->revision && vec_size(&directory->active) != 0u) {
    rc = flowie_cluster_owner_directory_entries_equal(&directory->active, entries, entry_count)
             ? TURBO_OK
             : TURBO_EPROTO;
    turbo_mutex_unlock(&directory->mutex);
    vec_clear(&directory->scratch);
    return rc;
  }
  {
    vec_t old = directory->active;
    directory->active = directory->scratch;
    directory->scratch = old;
    directory->revision = revision;
    directory->last_revision_shard = UINT32_MAX;
  }
  turbo_mutex_unlock(&directory->mutex);
  vec_clear(&directory->scratch);
  return TURBO_OK;
}

int flowie_cluster_owner_directory_apply(
    flowie_cluster_owner_directory_t *directory,
    const flowie_cluster_owner_directory_entry_t *entry, uint64_t revision) {
  flowie_cluster_owner_directory_entry_t *current;
  int equal;
  if (!directory || !entry || revision == 0u ||
      entry->shard_id >= directory->shard_count ||
      !flowie_cluster_owner_directory_entry_valid(entry, entry->shard_id))
    return TURBO_EINVAL;
  turbo_mutex_lock(&directory->mutex);
  if (revision < directory->revision) {
    turbo_mutex_unlock(&directory->mutex);
    return TURBO_EBUSY;
  }
  current = (flowie_cluster_owner_directory_entry_t *)vec_at(
      &directory->active, entry->shard_id);
  if (!current) {
    turbo_mutex_unlock(&directory->mutex);
    return TURBO_EPROTO;
  }
  if (revision == directory->revision) {
    equal = directory->last_revision_shard == entry->shard_id &&
            current->local_deadline_ns == entry->local_deadline_ns &&
            flowie_cluster_owner_directory_owner_equal(&current->owner,
                                                        &entry->owner);
    turbo_mutex_unlock(&directory->mutex);
    return equal ? TURBO_OK : TURBO_EPROTO;
  }
  *current = *entry;
  directory->revision = revision;
  directory->last_revision_shard = entry->shard_id;
  turbo_mutex_unlock(&directory->mutex);
  return TURBO_OK;
}

int flowie_cluster_owner_directory_resolve_shard(flowie_cluster_owner_directory_t *directory,
                                                 uint32_t shard_id,
                                                 flowie_cluster_owner_token_t *out) {
  const flowie_cluster_owner_directory_entry_t *entry;
  uint64_t now_ns;
  int rc = TURBO_EBUSY;
  if (out) *out = (flowie_cluster_owner_token_t)FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
  if (!directory || !out || shard_id >= directory->shard_count) return TURBO_EINVAL;
  now_ns = turbo_hrtime();
  turbo_mutex_lock(&directory->mutex);
  entry = (const flowie_cluster_owner_directory_entry_t *)vec_at_const(&directory->active,
                                                                             shard_id);
  if (entry && entry->local_deadline_ns > now_ns &&
      flowie_cluster_owner_directory_token_valid(&entry->owner, shard_id)) {
    *out = entry->owner;
    rc = TURBO_OK;
  }
  turbo_mutex_unlock(&directory->mutex);
  return rc;
}

int flowie_cluster_owner_directory_resolve(void *ctx, flowie_mqtt_span_t client_id,
                                           flowie_cluster_owner_token_t *out) {
  flowie_cluster_owner_directory_t *directory = (flowie_cluster_owner_directory_t *)ctx;
  uint32_t shard_id = 0u;
  int rc;
  if (!directory || !client_id.data || client_id.size == 0u || !out) return TURBO_EINVAL;
  rc = flowie_cluster_shard_for_key(
      directory->hash_version, FLOWIE_CLUSTER_KEY_SESSION,
      (const uint8_t *)directory->cluster_id, tstr_len(directory->cluster_id),
      (const uint8_t *)directory->listener_id, tstr_len(directory->listener_id), client_id.data,
      client_id.size, directory->shard_count, &shard_id);
  return rc == TURBO_OK ? flowie_cluster_owner_directory_resolve_shard(directory, shard_id, out)
                        : rc;
}

int flowie_cluster_owner_directory_revision(flowie_cluster_owner_directory_t *directory,
                                            uint64_t *out_revision) {
  if (out_revision) *out_revision = 0u;
  if (!directory || !out_revision) return TURBO_EINVAL;
  turbo_mutex_lock(&directory->mutex);
  *out_revision = directory->revision;
  turbo_mutex_unlock(&directory->mutex);
  return TURBO_OK;
}

void flowie_cluster_owner_directory_destroy(flowie_cluster_owner_directory_t *directory) {
  flowie_cluster_owner_directory_free(directory);
}
