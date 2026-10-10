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
  cnet_owner_placement_hint hints[2] = {{true, 4u}, {true, 1u}};
  cnet_owner_placement_input input = {0};
  size_t owner = SIZE_MAX;
  int (*set_destination)(flowie_mqtt_client_t *,
                         const flowie_mqtt_client_destination_policy_t *) =
      &flowie_mqtt_client_set_destination_policy;
  input.size = sizeof(input);
  input.version = CNET_OWNER_PLACEMENT_VERSION;
  input.kind = CNET_OWNER_PLACE_LOWEST_PRESSURE;
  input.owners = hints;
  input.owner_count = 2u;
  if (endpoint.size != sizeof(endpoint) || client.size != sizeof(client) ||
      packet.size != sizeof(packet) || set_destination == NULL) return 1;
  if (set_destination(NULL, NULL) != SALTS_EINVAL) return 2;
  if (flowie_mqtt_client_try_destroy(NULL, 0u) != SALTS_EINVAL) return 4;
  if (cnet_owner_placement_choose(&input, &owner) != SALTS_OK || owner != 1u) return 3;
  return 0;
}
