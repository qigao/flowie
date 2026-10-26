#include <flowie.h>
#include <flowie_mqtt_client.h>
#include <flowie_mqtt_protocol.h>
#include <cnet/owner_placement.h>
#include <salts/error_codes.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int main(void) {
  const flowie_endpoint_config_t endpoint = FLOWIE_ENDPOINT_CONFIG_INIT;
  const flowie_mqtt_client_config_t client = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  const flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  const flowie_mqtt_client_receive_policy_t receive_policy =
      FLOWIE_MQTT_CLIENT_RECEIVE_POLICY_INIT;
  const flowie_mqtt_client_inbound_token_t received_token = {0};
  cnet_owner_placement_hint hints[2] = {{true, 4u}, {true, 1u}};
  cnet_owner_placement_input input = {0};
  size_t owner = SIZE_MAX;
  static const uint8_t topic[] = "flowie/release";
  const flowie_mqtt_span_t topic_span = {topic, sizeof(topic) - 1u};
  int (*set_destination)(flowie_mqtt_client_t *,
                         const flowie_mqtt_client_destination_policy_t *) =
      &flowie_mqtt_client_set_destination_policy;
  int (*set_receive)(flowie_mqtt_client_t *,
                     const flowie_mqtt_client_receive_policy_t *) =
      &flowie_mqtt_client_set_receive_policy;
  int (*settle_qos1)(flowie_mqtt_client_t *,
                     const flowie_mqtt_client_inbound_token_t *,
                     flowie_mqtt_client_completion_fn, void *) =
      &flowie_mqtt_client_settle_inbound_qos1;
  input.size = sizeof(input);
  input.version = CNET_OWNER_PLACEMENT_VERSION;
  input.kind = CNET_OWNER_PLACE_LOWEST_PRESSURE;
  input.owners = hints;
  input.owner_count = 2u;
  if (endpoint.size != sizeof(endpoint) || client.size != sizeof(client) ||
      packet.size != sizeof(packet) || set_destination == NULL) return 1;
  if (set_destination(NULL, NULL) != SALTS_EINVAL) return 2;
  if (flowie_mqtt_client_try_destroy(NULL, 0u) != SALTS_EINVAL) return 4;
  /* Validate the newly installed public C ABI/symbols against this exact
   * dynamically linked Flowie::Client; no source-tree library fallback. */
  if (receive_policy.size != sizeof(receive_policy) ||
      receive_policy.version != FLOWIE_MQTT_CLIENT_RECEIVE_POLICY_VERSION ||
      received_token.session_generation != 0u || !set_receive ||
      !settle_qos1) return 7;
  if (set_receive(NULL, NULL) != SALTS_EINVAL ||
      settle_qos1(NULL, NULL, NULL, NULL) != SALTS_EINVAL) return 8;
  if (cnet_owner_placement_choose(&input, &owner) != SALTS_OK || owner != 1u) return 3;
  /* Exercise all installed library exports, not only header layout constants. */
  if (flowie_endpoint_core_stop(NULL) != SALTS_EINVAL) return 5;
  if (!flowie_mqtt_utf8_validate(topic_span)) return 6;
  return 0;
}
