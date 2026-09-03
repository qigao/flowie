#include "flowie_cluster_owner_projection_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <string.h>

static flowie_cluster_owner_directory_t *flowie_owner_projection_directory(void) {
  flowie_cluster_owner_directory_config_t config =
      FLOWIE_CLUSTER_OWNER_DIRECTORY_CONFIG_INIT;
  flowie_cluster_owner_directory_t *directory = NULL;
  config.shard_count = 2u;
  config.cluster_id = vstr_from_cstr("cluster-a");
  config.listener_id = vstr_from_cstr("mqtt");
  check_equal(flowie_cluster_owner_directory_create(&config, &directory),
               SALTS_OK);
  return directory;
}

spec("flowie cluster Raft owner projection") {
  it("projects committed assign and revoke commands by log index") {
    flowie_cluster_owner_directory_t *directory =
        flowie_owner_projection_directory();
    flowie_cluster_owner_projection_t projection = {directory};
    flowie_cluster_owner_command_t command = {0};
    flowie_cluster_owner_token_t owner = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
    tr_raft_entry_t entry = {0};
    size_t encoded_size = 0u;
    command.kind = FLOWIE_CLUSTER_OWNER_COMMAND_ASSIGN;
    command.shard_id = 1u;
    command.owner_epoch = 7u;
    command.node_id_size = strlen("node-b");
    memcpy(command.node_id, "node-b", command.node_id_size);
    command.boot_id[0] = 9u;
    check_equal(flowie_cluster_owner_command_encode(
                     &command, entry.data, sizeof(entry.data), &encoded_size),
                 SALTS_OK);
    entry.index = 11u;
    entry.data_length = encoded_size;
    check_equal(flowie_cluster_owner_projection_apply_batch(&projection,
                                                              &entry, 1u),
                 SALTS_OK);
    check_equal(flowie_cluster_owner_directory_resolve_shard(directory, 1u,
                                                               &owner),
                 SALTS_OK);
    check_equal(owner.owner_epoch, 7u);
    check_equal(owner.node_id, "node-b", strlen("node-b"));
    check_equal(flowie_cluster_owner_projection_apply_batch(&projection,
                                                              &entry, 1u),
                 SALTS_OK);

    memset(&command, 0, sizeof(command));
    command.kind = FLOWIE_CLUSTER_OWNER_COMMAND_REVOKE;
    command.shard_id = 1u;
    check_equal(flowie_cluster_owner_command_encode(
                     &command, entry.data, sizeof(entry.data), &encoded_size),
                 SALTS_OK);
    entry.index = 12u;
    entry.data_length = encoded_size;
    check_equal(flowie_cluster_owner_projection_apply_batch(&projection,
                                                              &entry, 1u),
                 SALTS_OK);
    check_equal(flowie_cluster_owner_directory_resolve_shard(directory, 1u,
                                                               &owner),
                 SALTS_EBUSY);
    flowie_cluster_owner_directory_destroy(directory);
  }

  it("rejects non-canonical and conflicting replay bytes") {
    flowie_cluster_owner_directory_t *directory =
        flowie_owner_projection_directory();
    flowie_cluster_owner_projection_t projection = {directory};
    flowie_cluster_owner_command_t command = {0};
    tr_raft_entry_t entry = {0};
    size_t encoded_size = 0u;
    command.kind = FLOWIE_CLUSTER_OWNER_COMMAND_REVOKE;
    command.shard_id = 0u;
    check_equal(flowie_cluster_owner_command_encode(
                     &command, entry.data, sizeof(entry.data), &encoded_size),
                 SALTS_OK);
    entry.index = 3u;
    entry.data_length = encoded_size;
    entry.data[6] = 1u;
    check_equal(flowie_cluster_owner_projection_apply_batch(&projection,
                                                              &entry, 1u),
                 SALTS_EPROTO);
    entry.data[6] = 0u;
    check_equal(flowie_cluster_owner_projection_apply_batch(&projection,
                                                              &entry, 1u),
                 SALTS_OK);
    entry.data[5] = FLOWIE_CLUSTER_OWNER_COMMAND_ASSIGN;
    check_equal(flowie_cluster_owner_projection_apply_batch(&projection,
                                                              &entry, 1u),
                 SALTS_EPROTO);
    flowie_cluster_owner_directory_destroy(directory);
  }
}
