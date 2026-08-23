#include "flowie_cluster_raft_generation_internal.h"

#include "tinytest.h"
#include "turbo_error.h"

#include <string.h>

typedef struct flowie_raft_generation_test_s {
  int events[10];
  size_t event_count;
  int drive_result;
  int retry_result;
  size_t proposal_count;
  uint64_t proposal_command_id;
  flowie_cluster_owner_command_t proposal_command;
  size_t publish_submit_count;
  tr_raft_term_t publish_term;
} flowie_raft_generation_test_t;

static flowie_raft_generation_test_t *flowie_raft_generation_test;

static void flowie_raft_generation_record(int event) {
  flowie_raft_generation_test_t *test = flowie_raft_generation_test;
  if (test && test->event_count < 10u)
    test->events[test->event_count++] = event;
}

static int flowie_raft_generation_owners_create(
    const flowie_cluster_owner_directory_config_t *config,
    flowie_cluster_owner_directory_t **out) {
  if (!config || !out) return TURBO_EINVAL;
  flowie_raft_generation_record(1);
  *out = (flowie_cluster_owner_directory_t *)flowie_raft_generation_test;
  return TURBO_OK;
}

static void flowie_raft_generation_owners_destroy(
    flowie_cluster_owner_directory_t *directory) {
  if (directory) flowie_raft_generation_record(10);
}

static int flowie_raft_generation_runtime_create(
    const flowie_cluster_raft_runtime_config_t *config,
    flowie_cluster_raft_runtime_t **out) {
  if (!config || !out) return TURBO_EINVAL;
  if (!config->store.state_machine.context ||
      config->store.state_machine.apply_batch !=
          flowie_cluster_state_machine_apply_batch)
    return TURBO_EINVAL;
  flowie_raft_generation_record(2);
  *out = (flowie_cluster_raft_runtime_t *)flowie_raft_generation_test;
  return TURBO_OK;
}

static int flowie_raft_generation_runtime_start(
    flowie_cluster_raft_runtime_t *runtime) {
  if (!runtime) return TURBO_EINVAL;
  flowie_raft_generation_record(4);
  return TURBO_OK;
}

static int flowie_raft_generation_runtime_drive(
    flowie_cluster_raft_runtime_t *runtime, uint32_t elapsed_ticks,
    uint32_t next_election_timeout_ticks,
    tr_raft_flowmq_peer_service_step_result_t *out_step) {
  if (!runtime || elapsed_ticks == 0u || next_election_timeout_ticks == 0u ||
      !out_step)
    return TURBO_EINVAL;
  flowie_raft_generation_record(5);
  return flowie_raft_generation_test->drive_result;
}

static int flowie_raft_generation_runtime_stop(
    flowie_cluster_raft_runtime_t *runtime) {
  if (!runtime) return TURBO_EINVAL;
  flowie_raft_generation_record(7);
  return TURBO_OK;
}

static int flowie_raft_generation_runtime_status(
    const flowie_cluster_raft_runtime_t *runtime,
    flowie_cluster_raft_runtime_status_t *out_status) {
  if (!runtime || !out_status) return TURBO_EINVAL;
  memset(out_status, 0, sizeof(*out_status));
  out_status->raft.core.role = TR_RAFT_LEADER;
  out_status->raft.core.term = 3u;
  return TURBO_OK;
}

static int flowie_raft_generation_runtime_configuration(
    const flowie_cluster_raft_runtime_t *runtime,
    tr_raft_conf_t *out_configuration) {
  if (!runtime || !out_configuration) return TURBO_EINVAL;
  memset(out_configuration, 0, sizeof(*out_configuration));
  out_configuration->phase = TR_RAFT_CONF_FINAL;
  out_configuration->member_count = 1u;
  out_configuration->members[0].node_id = 1u;
  out_configuration->members[0].roles =
      TR_RAFT_CONF_OLD_VOTER | TR_RAFT_CONF_NEW_VOTER;
  return TURBO_OK;
}

static int flowie_raft_generation_runtime_propose(
    flowie_cluster_raft_runtime_t *runtime,
    const tr_raft_proposal_t *proposal,
    tr_raft_operation_status_t *out_receipt) {
  flowie_raft_generation_test_t *test = flowie_raft_generation_test;
  if (!runtime || !proposal || !out_receipt || !test) return TURBO_EINVAL;
  if (flowie_cluster_owner_command_decode(
          (const uint8_t *)proposal->data, proposal->data_length,
          &test->proposal_command) != TURBO_OK)
    return TURBO_EPROTO;
  ++test->proposal_count;
  test->proposal_command_id = proposal->command_id;
  return TURBO_OK;
}

static int flowie_raft_generation_runtime_destroy(
    flowie_cluster_raft_runtime_t *runtime) {
  if (!runtime) return TURBO_EINVAL;
  flowie_raft_generation_record(9);
  return TURBO_OK;
}

static int flowie_raft_generation_router_create(
    const flowie_cluster_publish_router_config_t *config,
    flowie_cluster_raft_runtime_t *runtime,
    flowie_cluster_publish_router_t **out) {
  if (!config || !runtime || !out) return TURBO_EINVAL;
  flowie_raft_generation_record(3);
  *out = (flowie_cluster_publish_router_t *)flowie_raft_generation_test;
  return TURBO_OK;
}

static int flowie_raft_generation_router_retry(
    flowie_cluster_publish_router_t *router) {
  if (!router) return TURBO_EINVAL;
  flowie_raft_generation_record(6);
  return flowie_raft_generation_test->retry_result;
}

static int flowie_raft_generation_router_destroy(
    flowie_cluster_publish_router_t *router) {
  if (!router) return TURBO_EINVAL;
  flowie_raft_generation_record(8);
  return TURBO_OK;
}

static int flowie_raft_generation_router_submit(
    flowie_cluster_publish_router_t *router, tr_raft_term_t term,
    uint64_t stream_id, uint64_t command_id,
    const tr_raft_conf_t *configuration, tstr *event) {
  flowie_raft_generation_test_t *test = flowie_raft_generation_test;
  if (!router || term == 0u || stream_id == 0u || command_id == 0u ||
      !configuration || !event || !*event || !test)
    return TURBO_EINVAL;
  ++test->publish_submit_count;
  test->publish_term = term;
  tstr_freep(event);
  return TURBO_OK;
}

static const flowie_cluster_raft_generation_api_t TEST_API = {
    flowie_raft_generation_owners_create,
    flowie_raft_generation_owners_destroy,
    flowie_raft_generation_runtime_create,
    flowie_raft_generation_runtime_start,
    flowie_raft_generation_runtime_drive,
    flowie_raft_generation_runtime_propose,
    flowie_raft_generation_runtime_status,
    flowie_raft_generation_runtime_configuration,
    flowie_raft_generation_runtime_stop,
    flowie_raft_generation_runtime_destroy,
    flowie_raft_generation_router_create,
    flowie_raft_generation_router_retry,
    flowie_raft_generation_router_submit,
    flowie_raft_generation_router_destroy};

static int flowie_raft_generation_apply_publish(
    void *ctx, tr_raft_index_t index, tr_raft_term_t term,
    uint64_t command_id, const tr_raft_data_descriptor_t *descriptor) {
  (void)ctx;
  (void)index;
  (void)term;
  (void)command_id;
  return descriptor ? TURBO_OK : TURBO_EINVAL;
}

spec("flowie cluster Raft generation") {
  it("owns runtime and router in dependency order") {
    flowie_raft_generation_test_t test = {0};
    flowie_cluster_raft_generation_config_t config = {0};
    flowie_cluster_raft_generation_t *generation = NULL;
    tr_raft_flowmq_peer_service_step_result_t step = {0};
    tr_raft_operation_status_t receipt = {0};
    flowie_cluster_owner_command_t owner_command = {0};
    static const uint8_t publish_packet[] = {0x30u, 0x07u, 0x00u, 0x01u, 'a',
                                             0x00u, 'o',   'k',   '!'};
    static const uint8_t client_id[] = "client-a";
    uint8_t edge_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {1u};
    flowie_cluster_raft_publish_request_t publish = {0};
    tstr event = NULL;
    flowie_raft_generation_test = &test;
    config.apply_publish = flowie_raft_generation_apply_publish;
    config.router.max_event_bytes = 4096u;
    check_equal(flowie_cluster_raft_generation_create_with_api(
                     &config, &TEST_API, &generation),
                 TURBO_OK);
    check_not_null(flowie_cluster_raft_generation_owners(generation));
    check_equal(flowie_cluster_raft_generation_start(generation), TURBO_OK);
    owner_command.kind = FLOWIE_CLUSTER_OWNER_COMMAND_REVOKE;
    owner_command.shard_id = 1u;
    check_equal(flowie_cluster_raft_generation_propose_owner(
                     generation, 71u, &owner_command, &receipt),
                 TURBO_OK);
    check_equal(test.proposal_count, 1u);
    check_equal(test.proposal_command_id, 71u);
    check_equal(test.proposal_command.kind,
                 FLOWIE_CLUSTER_OWNER_COMMAND_REVOKE);
    check_equal(test.proposal_command.shard_id, 1u);
    event = tstr_new_len("durable-event", strlen("durable-event"));
    check_not_null(event);
    check_equal(flowie_cluster_raft_generation_submit_publish_durable(
                     generation, 72u, 73u, &event),
                 TURBO_OK);
    check_null(event);
    check_equal(test.publish_submit_count, 1u);
    check_equal(test.publish_term, 3u);
    publish.stream_id = 74u;
    publish.command_id = 75u;
    publish.mqtt_version = FLOWIE_MQTT_VERSION_5;
    publish.requested_settlement = FLOWIE_PROTOCOL_SETTLE_RECEIVED;
    publish.connection_id = 10u;
    publish.connection_generation = 11u;
    publish.session_id = 12u;
    publish.session_generation = 13u;
    publish.accepted_at_epoch_seconds = 14u;
    publish.edge_node_id = vstr_from_cstr("edge-a");
    publish.edge_boot_id = edge_boot_id;
    publish.client_id =
        (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    publish.packet =
        (flowie_mqtt_span_t){publish_packet, sizeof(publish_packet)};
    check_equal(flowie_cluster_raft_generation_publish(generation, &publish),
                 TURBO_OK);
    check_equal(test.publish_submit_count, 2u);
    check_equal(flowie_cluster_raft_generation_drive(generation, 1u, 3u,
                                                      &step),
                 TURBO_OK);
    check_equal(flowie_cluster_raft_generation_destroy(generation),
                 TURBO_EBUSY);
    check_equal(flowie_cluster_raft_generation_stop(generation), TURBO_OK);
    check_equal(flowie_cluster_raft_generation_destroy(generation), TURBO_OK);
    check_equal(test.event_count, 10u);
    for (size_t index = 0u; index < test.event_count; ++index)
      check_equal(test.events[index], (int)index + 1);
    flowie_raft_generation_test = NULL;
  }

  it("does not retry router work after a failed Raft drive") {
    flowie_raft_generation_test_t test = {0};
    flowie_cluster_raft_generation_config_t config = {0};
    flowie_cluster_raft_generation_t *generation = NULL;
    tr_raft_flowmq_peer_service_step_result_t step = {0};
    test.drive_result = TURBO_EIO;
    flowie_raft_generation_test = &test;
    config.apply_publish = flowie_raft_generation_apply_publish;
    config.router.max_event_bytes = 4096u;
    check_equal(flowie_cluster_raft_generation_create_with_api(
                     &config, &TEST_API, &generation),
                 TURBO_OK);
    check_equal(flowie_cluster_raft_generation_start(generation), TURBO_OK);
    check_equal(flowie_cluster_raft_generation_drive(generation, 1u, 3u,
                                                      &step),
                 TURBO_EIO);
    check_equal(test.event_count, 5u);
    check_equal(flowie_cluster_raft_generation_stop(generation), TURBO_OK);
    check_equal(flowie_cluster_raft_generation_destroy(generation), TURBO_OK);
    flowie_raft_generation_test = NULL;
  }
}
