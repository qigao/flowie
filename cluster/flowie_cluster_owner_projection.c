#include "flowie_cluster_owner_projection_internal.h"

#include "salts_error.h"

#include <string.h>

enum {
  FLOWIE_OWNER_OFFSET_MAGIC = 0u,
  FLOWIE_OWNER_OFFSET_VERSION = 4u,
  FLOWIE_OWNER_OFFSET_KIND = 5u,
  FLOWIE_OWNER_OFFSET_RESERVED = 6u,
  FLOWIE_OWNER_OFFSET_SHARD = 8u,
  FLOWIE_OWNER_OFFSET_EPOCH = 12u,
  FLOWIE_OWNER_OFFSET_NODE_SIZE = 20u,
  FLOWIE_OWNER_OFFSET_RESERVED_2 = 22u,
  FLOWIE_OWNER_OFFSET_BOOT = 24u,
  FLOWIE_OWNER_OFFSET_NODE = 40u
};

static const uint8_t FLOWIE_OWNER_COMMAND_MAGIC[4] = {'F', 'W', 'O', 'R'};

static void flowie_owner_write_u16(uint8_t *output, uint16_t value) {
  output[0] = (uint8_t)value;
  output[1] = (uint8_t)(value >> 8u);
}

static void flowie_owner_write_u32(uint8_t *output, uint32_t value) {
  for (size_t index = 0u; index < 4u; ++index)
    output[index] = (uint8_t)(value >> (index * 8u));
}

static void flowie_owner_write_u64(uint8_t *output, uint64_t value) {
  for (size_t index = 0u; index < 8u; ++index)
    output[index] = (uint8_t)(value >> (index * 8u));
}

static uint16_t flowie_owner_read_u16(const uint8_t *input) {
  return (uint16_t)input[0] | (uint16_t)((uint16_t)input[1] << 8u);
}

static uint32_t flowie_owner_read_u32(const uint8_t *input) {
  uint32_t value = 0u;
  for (size_t index = 0u; index < 4u; ++index)
    value |= (uint32_t)input[index] << (index * 8u);
  return value;
}

static uint64_t flowie_owner_read_u64(const uint8_t *input) {
  uint64_t value = 0u;
  for (size_t index = 0u; index < 8u; ++index)
    value |= (uint64_t)input[index] << (index * 8u);
  return value;
}

static int flowie_owner_boot_nonzero(
    const uint8_t boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE]) {
  uint8_t combined = 0u;
  for (size_t index = 0u; index < FLOWIE_CLUSTER_BOOT_ID_SIZE; ++index)
    combined |= boot_id[index];
  return combined != 0u;
}

static int flowie_owner_command_valid(
    const flowie_cluster_owner_command_t *command) {
  if (!command || command->shard_id >= FLOWIE_CLUSTER_SHARD_COUNT_MAX)
    return 0;
  if (command->kind == FLOWIE_CLUSTER_OWNER_COMMAND_REVOKE)
    return command->owner_epoch == 0u && command->node_id_size == 0u &&
           !flowie_owner_boot_nonzero(command->boot_id);
  return command->kind == FLOWIE_CLUSTER_OWNER_COMMAND_ASSIGN &&
         command->owner_epoch != 0u && command->node_id_size != 0u &&
         command->node_id_size <= FLOWIE_CLUSTER_NODE_ID_MAX &&
         !memchr(command->node_id, '\0', command->node_id_size) &&
         flowie_owner_boot_nonzero(command->boot_id);
}

int flowie_cluster_owner_command_encode(
    const flowie_cluster_owner_command_t *command, uint8_t *output,
    size_t output_capacity, size_t *output_size) {
  size_t encoded_size;
  if (output_size) *output_size = 0u;
  if (!flowie_owner_command_valid(command) || !output || !output_size)
    return SALTS_EINVAL;
  encoded_size = FLOWIE_CLUSTER_OWNER_COMMAND_HEADER_SIZE +
                 command->node_id_size;
  if (output_capacity < encoded_size) return SALTS_ENOSPC;
  memset(output, 0, encoded_size);
  memcpy(output + FLOWIE_OWNER_OFFSET_MAGIC, FLOWIE_OWNER_COMMAND_MAGIC,
         sizeof(FLOWIE_OWNER_COMMAND_MAGIC));
  output[FLOWIE_OWNER_OFFSET_VERSION] = FLOWIE_CLUSTER_OWNER_COMMAND_VERSION;
  output[FLOWIE_OWNER_OFFSET_KIND] = (uint8_t)command->kind;
  flowie_owner_write_u32(output + FLOWIE_OWNER_OFFSET_SHARD,
                         command->shard_id);
  flowie_owner_write_u64(output + FLOWIE_OWNER_OFFSET_EPOCH,
                         command->owner_epoch);
  flowie_owner_write_u16(output + FLOWIE_OWNER_OFFSET_NODE_SIZE,
                         (uint16_t)command->node_id_size);
  memcpy(output + FLOWIE_OWNER_OFFSET_BOOT, command->boot_id,
         FLOWIE_CLUSTER_BOOT_ID_SIZE);
  memcpy(output + FLOWIE_OWNER_OFFSET_NODE, command->node_id,
         command->node_id_size);
  *output_size = encoded_size;
  return SALTS_OK;
}

int flowie_cluster_owner_command_decode(
    const uint8_t *input, size_t input_size,
    flowie_cluster_owner_command_t *out) {
  flowie_cluster_owner_command_t command;
  uint16_t node_id_size;
  if (out) memset(out, 0, sizeof(*out));
  if (!input || !out || input_size < FLOWIE_CLUSTER_OWNER_COMMAND_HEADER_SIZE ||
      memcmp(input + FLOWIE_OWNER_OFFSET_MAGIC, FLOWIE_OWNER_COMMAND_MAGIC,
             sizeof(FLOWIE_OWNER_COMMAND_MAGIC)) != 0 ||
      input[FLOWIE_OWNER_OFFSET_VERSION] != FLOWIE_CLUSTER_OWNER_COMMAND_VERSION ||
      input[FLOWIE_OWNER_OFFSET_RESERVED] != 0u ||
      input[FLOWIE_OWNER_OFFSET_RESERVED + 1u] != 0u ||
      input[FLOWIE_OWNER_OFFSET_RESERVED_2] != 0u ||
      input[FLOWIE_OWNER_OFFSET_RESERVED_2 + 1u] != 0u)
    return SALTS_EPROTO;
  node_id_size = flowie_owner_read_u16(input + FLOWIE_OWNER_OFFSET_NODE_SIZE);
  if (node_id_size > FLOWIE_CLUSTER_NODE_ID_MAX ||
      input_size != FLOWIE_CLUSTER_OWNER_COMMAND_HEADER_SIZE + node_id_size)
    return SALTS_EPROTO;
  memset(&command, 0, sizeof(command));
  command.kind = (flowie_cluster_owner_command_kind_t)
      input[FLOWIE_OWNER_OFFSET_KIND];
  command.shard_id = flowie_owner_read_u32(input + FLOWIE_OWNER_OFFSET_SHARD);
  command.owner_epoch = flowie_owner_read_u64(input + FLOWIE_OWNER_OFFSET_EPOCH);
  command.node_id_size = node_id_size;
  memcpy(command.boot_id, input + FLOWIE_OWNER_OFFSET_BOOT,
         FLOWIE_CLUSTER_BOOT_ID_SIZE);
  memcpy(command.node_id, input + FLOWIE_OWNER_OFFSET_NODE, node_id_size);
  command.node_id[node_id_size] = '\0';
  if (!flowie_owner_command_valid(&command)) return SALTS_EPROTO;
  *out = command;
  return SALTS_OK;
}

int flowie_cluster_owner_projection_apply_batch(
    void *ctx, const tr_raft_entry_t *entries, size_t entry_count) {
  flowie_cluster_owner_projection_t *projection =
      (flowie_cluster_owner_projection_t *)ctx;
  size_t index;
  if (!projection || !projection->directory || !entries || entry_count == 0u)
    return SALTS_EINVAL;
  for (index = 0u; index < entry_count; ++index) {
    flowie_cluster_owner_command_t command;
    flowie_cluster_owner_directory_entry_t directory_entry =
        FLOWIE_CLUSTER_OWNER_DIRECTORY_ENTRY_INIT;
    int rc;
    if (entries[index].index == 0u) return SALTS_EPROTO;
    rc = flowie_cluster_owner_command_decode(entries[index].data,
                                             entries[index].data_length,
                                             &command);
    if (rc != SALTS_OK) return rc;
    directory_entry.shard_id = command.shard_id;
    directory_entry.owner.shard_id = command.shard_id;
    if (command.kind == FLOWIE_CLUSTER_OWNER_COMMAND_ASSIGN) {
      directory_entry.local_deadline_ns = UINT64_MAX;
      rc = flowie_cluster_owner_token_init(
          &directory_entry.owner, command.shard_id, command.owner_epoch,
          command.node_id, command.node_id_size, command.boot_id);
      if (rc != SALTS_OK) return rc;
    }
    rc = flowie_cluster_owner_directory_apply(
        projection->directory, &directory_entry, entries[index].index);
    if (rc != SALTS_OK) return rc;
  }
  return SALTS_OK;
}
