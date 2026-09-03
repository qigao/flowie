#include "flowie_cluster_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <stdint.h>
#include <string.h>

static flowie_cluster_config_t flowie_cluster_test_config(void) {
  flowie_cluster_config_t config = FLOWIE_CLUSTER_CONFIG_INIT;
  config.shard_count = 256u;
  config.max_nodes = 8u;
  config.lease_ttl_ms = 15000u;
  config.renew_interval_ms = 3000u;
  config.worst_case_db_latency_ms = 1000u;
  config.safety_margin_ms = 1000u;
  config.peer_queue_entries = 4096u;
  config.peer_queue_bytes = 16u * 1024u * 1024u;
  config.max_command_bytes = 1024u * 1024u;
  config.outbox_records = 100000u;
  config.outbox_bytes = 1024u * 1024u * 1024u;
  return config;
}

static int flowie_cluster_test_shard(flowie_cluster_key_kind_t kind, const char *key,
                                     uint32_t shard_count, uint32_t *out) {
  static const uint8_t cluster_id[] = "cluster-a";
  static const uint8_t listener_id[] = "mqtt-main";
  return flowie_cluster_shard_for_key(
      FLOWIE_CLUSTER_HASH_VERSION_1, kind, cluster_id, sizeof(cluster_id) - 1u, listener_id,
      sizeof(listener_id) - 1u, (const uint8_t *)key, strlen(key), shard_count, out);
}

spec("flowie cluster protocol kernel") {
  group("configuration") {
    it("accepts bounded lease and queue limits") {
      flowie_cluster_config_t config = flowie_cluster_test_config();
      check_equal(flowie_cluster_config_validate(&config), SALTS_OK);
    }

    it("rejects a renewal budget that reaches the lease ttl") {
      flowie_cluster_config_t config = flowie_cluster_test_config();
      config.renew_interval_ms = 13000u;
      check_equal(flowie_cluster_config_validate(&config), SALTS_EINVAL);
      config.renew_interval_ms = UINT64_MAX;
      check_equal(flowie_cluster_config_validate(&config), SALTS_ERANGE);
    }

    it("rejects unbounded or internally inconsistent capacities") {
      flowie_cluster_config_t config = flowie_cluster_test_config();
      config.peer_queue_entries = 0u;
      check_equal(flowie_cluster_config_validate(&config), SALTS_EINVAL);
      config = flowie_cluster_test_config();
      config.max_command_bytes = config.peer_queue_bytes + 1u;
      check_equal(flowie_cluster_config_validate(&config), SALTS_EINVAL);
      config = flowie_cluster_test_config();
      config.shard_count = FLOWIE_CLUSTER_SHARD_COUNT_MAX + 1u;
      check_equal(flowie_cluster_config_validate(&config), SALTS_EINVAL);
    }
  }

  group("stable shard mapping") {
    it("maps identical canonical keys deterministically") {
      uint32_t first = 0u;
      uint32_t second = 0u;
      check_equal(flowie_cluster_test_shard(FLOWIE_CLUSTER_KEY_SESSION, "client-a", 256u, &first),
                   SALTS_OK);
      check_equal(flowie_cluster_test_shard(FLOWIE_CLUSTER_KEY_SESSION, "client-a", 256u, &second),
                   SALTS_OK);
      check_equal(first, second);
      check_equal(first, 58u);
      check_true(first < 256u);
    }

    it("separates session and retained key domains") {
      uint32_t session = 0u;
      uint32_t retained = 0u;
      check_equal(
          flowie_cluster_test_shard(FLOWIE_CLUSTER_KEY_SESSION, "same-key", 65521u, &session),
          SALTS_OK);
      check_equal(
          flowie_cluster_test_shard(FLOWIE_CLUSTER_KEY_RETAINED, "same-key", 65521u, &retained),
          SALTS_OK);
      check_not_equal(session, retained);
      check_equal(session, 517u);
      check_equal(retained, 4363u);
    }

    it("rejects empty keys unsupported versions and invalid shard counts") {
      static const uint8_t cluster_id[] = "cluster-a";
      static const uint8_t listener_id[] = "mqtt-main";
      uint32_t shard = 99u;
      check_equal(flowie_cluster_shard_for_key(FLOWIE_CLUSTER_HASH_VERSION_1,
                                                FLOWIE_CLUSTER_KEY_SESSION, cluster_id,
                                                sizeof(cluster_id) - 1u, listener_id,
                                                sizeof(listener_id) - 1u, NULL, 0u, 16u, &shard),
                   SALTS_EINVAL);
      check_equal(shard, 0u);
      check_equal(flowie_cluster_shard_for_key(
                       FLOWIE_CLUSTER_HASH_VERSION_1 + 1u, FLOWIE_CLUSTER_KEY_SESSION, cluster_id,
                       sizeof(cluster_id) - 1u, listener_id, sizeof(listener_id) - 1u,
                       (const uint8_t *)"a", 1u, 16u, &shard),
                   SALTS_EINVAL);
      check_equal(flowie_cluster_test_shard(FLOWIE_CLUSTER_KEY_SESSION, "client-a", 0u, &shard),
                   SALTS_EINVAL);
    }
  }

  group("local runtime adapter") {
    it("assigns every key to one immutable local owner") {
      static const uint8_t client_id[] = "client-a";
      static const uint8_t topic[] = "devices/one/state";
      flowie_cluster_runtime_t *runtime = NULL;
      flowie_cluster_owner_token_t session_owner = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
      flowie_cluster_owner_token_t retained_owner = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
      check_equal(flowie_cluster_runtime_create_local(7u, &runtime), SALTS_OK);
      check_not_null(runtime);
      check_equal(flowie_cluster_runtime_owner_for_key(runtime, FLOWIE_CLUSTER_KEY_SESSION,
                                                        client_id, sizeof(client_id) - 1u,
                                                        &session_owner),
                   SALTS_OK);
      check_equal(flowie_cluster_runtime_owner_for_key(runtime, FLOWIE_CLUSTER_KEY_RETAINED, topic,
                                                        sizeof(topic) - 1u, &retained_owner),
                   SALTS_OK);
      check_equal(session_owner.shard_id, 0u);
      check_equal(session_owner.owner_epoch, 1u);
      check_equal(session_owner.node_id, "local-7");
      check_equal(flowie_cluster_owner_token_require(&session_owner, &retained_owner), SALTS_OK);
      flowie_cluster_runtime_destroy(runtime);
    }

    it("rejects invalid creation and key inputs without leaking an output token") {
      flowie_cluster_runtime_t *runtime = (flowie_cluster_runtime_t *)(uintptr_t)1u;
      flowie_cluster_owner_token_t owner = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
      owner.owner_epoch = 99u;
      check_equal(flowie_cluster_runtime_create_local(0u, &runtime), SALTS_EINVAL);
      check_null(runtime);
      check_equal(flowie_cluster_runtime_create_local(8u, &runtime), SALTS_OK);
      check_equal(flowie_cluster_runtime_owner_for_key(runtime, FLOWIE_CLUSTER_KEY_SESSION, NULL,
                                                        0u, &owner),
                   SALTS_EINVAL);
      check_equal(owner.owner_epoch, 0u);
      flowie_cluster_runtime_destroy(runtime);
      flowie_cluster_runtime_destroy(NULL);
    }
  }

  group("state transitions") {
    it("admits the normal node shard and connection lifecycles") {
      check_equal(flowie_cluster_node_transition_validate(FLOWIE_CLUSTER_NODE_STARTING,
                                                           FLOWIE_CLUSTER_NODE_SYNCING),
                   SALTS_OK);
      check_equal(flowie_cluster_node_transition_validate(FLOWIE_CLUSTER_NODE_SYNCING,
                                                           FLOWIE_CLUSTER_NODE_READY),
                   SALTS_OK);
      check_equal(flowie_cluster_node_transition_validate(FLOWIE_CLUSTER_NODE_READY,
                                                           FLOWIE_CLUSTER_NODE_DRAINING),
                   SALTS_OK);
      check_equal(flowie_cluster_shard_transition_validate(FLOWIE_CLUSTER_SHARD_UNASSIGNED,
                                                            FLOWIE_CLUSTER_SHARD_CLAIMING),
                   SALTS_OK);
      check_equal(flowie_cluster_shard_transition_validate(FLOWIE_CLUSTER_SHARD_CLAIMING,
                                                            FLOWIE_CLUSTER_SHARD_RECOVERING),
                   SALTS_OK);
      check_equal(flowie_cluster_shard_transition_validate(FLOWIE_CLUSTER_SHARD_RECOVERING,
                                                            FLOWIE_CLUSTER_SHARD_ACTIVE),
                   SALTS_OK);
      check_equal(
          flowie_cluster_connection_transition_validate(FLOWIE_CLUSTER_CONNECTION_ACCEPTED,
                                                        FLOWIE_CLUSTER_CONNECTION_AUTHENTICATING),
          SALTS_OK);
      check_equal(flowie_cluster_connection_transition_validate(
                       FLOWIE_CLUSTER_CONNECTION_AUTHENTICATING, FLOWIE_CLUSTER_CONNECTION_BINDING),
                   SALTS_OK);
      check_equal(flowie_cluster_connection_transition_validate(FLOWIE_CLUSTER_CONNECTION_BINDING,
                                                                 FLOWIE_CLUSTER_CONNECTION_ACTIVE),
                   SALTS_OK);
    }

    it("rejects skipped terminal and repeated transitions") {
      check_equal(flowie_cluster_node_transition_validate(FLOWIE_CLUSTER_NODE_STARTING,
                                                           FLOWIE_CLUSTER_NODE_READY),
                   SALTS_EBUSY);
      check_equal(flowie_cluster_shard_transition_validate(FLOWIE_CLUSTER_SHARD_UNASSIGNED,
                                                            FLOWIE_CLUSTER_SHARD_ACTIVE),
                   SALTS_EBUSY);
      check_equal(flowie_cluster_connection_transition_validate(FLOWIE_CLUSTER_CONNECTION_CLOSED,
                                                                 FLOWIE_CLUSTER_CONNECTION_ACTIVE),
                   SALTS_EBUSY);
      check_equal(flowie_cluster_connection_transition_validate(FLOWIE_CLUSTER_CONNECTION_ACTIVE,
                                                                 FLOWIE_CLUSTER_CONNECTION_ACTIVE),
                   SALTS_EALREADY);
    }

    it("forces a fenced shard through unassigned before a new claim") {
      check_equal(flowie_cluster_shard_transition_validate(FLOWIE_CLUSTER_SHARD_ACTIVE,
                                                            FLOWIE_CLUSTER_SHARD_FENCED),
                   SALTS_OK);
      check_equal(flowie_cluster_shard_transition_validate(FLOWIE_CLUSTER_SHARD_FENCED,
                                                            FLOWIE_CLUSTER_SHARD_CLAIMING),
                   SALTS_EBUSY);
      check_equal(flowie_cluster_shard_transition_validate(FLOWIE_CLUSTER_SHARD_FENCED,
                                                            FLOWIE_CLUSTER_SHARD_UNASSIGNED),
                   SALTS_OK);
    }
  }

  group("lease and capacity arithmetic") {
    it("computes a conservative monotonic deadline") {
      uint64_t deadline = 0u;
      check_equal(flowie_cluster_lease_deadline_ns(UINT64_C(1000000000), 15000u, 1000u, &deadline),
                   SALTS_OK);
      check_equal(deadline, UINT64_C(15000000000));
    }

    it("rejects expired and overflowing lease deadlines") {
      uint64_t deadline = 1u;
      check_equal(flowie_cluster_lease_deadline_ns(0u, 1000u, 1000u, &deadline), SALTS_EINVAL);
      check_equal(deadline, 0u);
      check_equal(flowie_cluster_lease_deadline_ns(UINT64_MAX - 10u, 2u, 1u, &deadline),
                   SALTS_ERANGE);
    }

    it("calculates queue entries with ceiling and checked arithmetic") {
      uint64_t entries = 0u;
      check_equal(flowie_cluster_required_queue_entries(1501u, 250u, 32u, &entries), SALTS_OK);
      check_equal(entries, 408u);
      check_equal(flowie_cluster_required_queue_entries(UINT64_MAX, 2u, 0u, &entries),
                   SALTS_ERANGE);
    }
  }

  group("fencing token") {
    it("accepts only an exact node boot shard and epoch match") {
      static const uint8_t boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {
          1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u};
      flowie_cluster_owner_token_t expected = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
      flowie_cluster_owner_token_t presented = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
      check_equal(flowie_cluster_owner_token_init(&expected, 7u, 42u, "node-a", 6u, boot_id),
                   SALTS_OK);
      presented = expected;
      check_equal(flowie_cluster_owner_token_require(&expected, &presented), SALTS_OK);
      presented.owner_epoch = 41u;
      check_equal(flowie_cluster_owner_token_require(&expected, &presented), SALTS_EBUSY);
      presented = expected;
      presented.boot_id[0] ^= 1u;
      check_equal(flowie_cluster_owner_token_require(&expected, &presented), SALTS_EBUSY);
    }

    it("rejects malformed tokens before comparing authority") {
      static const uint8_t zero_boot[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {0u};
      flowie_cluster_owner_token_t token = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
      check_equal(flowie_cluster_owner_token_init(&token, 1u, 1u, "node", 4u, zero_boot),
                   SALTS_EINVAL);
      check_equal(flowie_cluster_owner_token_require(&token, &token), SALTS_EINVAL);
    }
  }

  group("authoritative shard lease") {
    it("claims renews authorizes and releases one epoch") {
      static const uint8_t boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u,
                                                                   1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u};
      flowie_cluster_shard_lease_t lease = FLOWIE_CLUSTER_SHARD_LEASE_INIT;
      flowie_cluster_owner_token_t token = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
      uint64_t lease_until = 0u;
      check_equal(
          flowie_cluster_shard_lease_claim(&lease, 1000u, 5000u, 9u, "node-a", 6u, boot_id, &token),
          SALTS_OK);
      check_equal(token.owner_epoch, 1u);
      check_equal(lease.lease_until_db_ms, 6000u);
      check_equal(flowie_cluster_shard_lease_require(&lease, 5999u, &token), SALTS_OK);
      check_equal(flowie_cluster_shard_lease_renew(&lease, 2000u, 5000u, &token, &lease_until),
                   SALTS_OK);
      check_equal(lease_until, 7000u);
      check_equal(flowie_cluster_shard_lease_release(&lease, 3000u, &token), SALTS_OK);
      check_false(lease.owned);
      check_equal(lease.owner.owner_epoch, 1u);
    }

    it("fences stale owners after expiry and epoch advancement") {
      static const uint8_t boot_a[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {1u};
      static const uint8_t boot_b[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {2u};
      flowie_cluster_shard_lease_t lease = FLOWIE_CLUSTER_SHARD_LEASE_INIT;
      flowie_cluster_owner_token_t stale = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
      flowie_cluster_owner_token_t current = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
      check_equal(
          flowie_cluster_shard_lease_claim(&lease, 100u, 50u, 3u, "node-a", 6u, boot_a, &stale),
          SALTS_OK);
      check_equal(
          flowie_cluster_shard_lease_claim(&lease, 149u, 50u, 3u, "node-b", 6u, boot_b, &current),
          SALTS_EBUSY);
      check_equal(
          flowie_cluster_shard_lease_claim(&lease, 150u, 50u, 3u, "node-b", 6u, boot_b, &current),
          SALTS_OK);
      check_equal(current.owner_epoch, 2u);
      check_equal(flowie_cluster_shard_lease_require(&lease, 151u, &stale), SALTS_EBUSY);
      check_equal(flowie_cluster_shard_lease_release(&lease, 151u, &stale), SALTS_EBUSY);
      check_equal(flowie_cluster_shard_lease_require(&lease, 151u, &current), SALTS_OK);
    }

    it("does not mutate a lease when deadline or epoch arithmetic overflows") {
      static const uint8_t boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {1u};
      flowie_cluster_shard_lease_t lease = FLOWIE_CLUSTER_SHARD_LEASE_INIT;
      flowie_cluster_owner_token_t token = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
      check_equal(flowie_cluster_shard_lease_claim(&lease, UINT64_MAX - 1u, 2u, 1u, "node", 4u,
                                                    boot_id, &token),
                   SALTS_ERANGE);
      check_false(lease.owned);
      lease.owned = 1u;
      lease.lease_until_db_ms = 10u;
      check_equal(
          flowie_cluster_owner_token_init(&lease.owner, 1u, UINT64_MAX, "node", 4u, boot_id),
          SALTS_OK);
      check_equal(
          flowie_cluster_shard_lease_claim(&lease, 10u, 1u, 1u, "node", 4u, boot_id, &token),
          SALTS_ERANGE);
      check_equal(lease.owner.owner_epoch, UINT64_MAX);
    }
  }
}
