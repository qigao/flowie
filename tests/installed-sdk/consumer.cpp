#include <flowie.h>
#include <flowie_mqtt_client.h>
#include <flowie_mqtt_protocol.h>
#include <cnet/owner_placement.h>
#include <salts/error_codes.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(std::is_standard_layout<flowie_mqtt_client_destination_policy_t>::value,
              "Client destination config must remain a C-compatible layout");
static_assert(std::is_standard_layout<flowie_endpoint_config_t>::value,
              "Broker endpoint must remain a C-compatible layout");

int main() {
  flowie_mqtt_client_destination_policy_t policy{};
  flowie_mqtt_client_remote_endpoint_t endpoint{};
  cnet_owner_placement_hint hints[2] = {{true, 3u}, {true, 0u}};
  cnet_owner_placement_input input{};
  size_t owner = SIZE_MAX;
  using SetDestination = int (*)(flowie_mqtt_client_t *,
                                const flowie_mqtt_client_destination_policy_t *);
  SetDestination set_destination = &flowie_mqtt_client_set_destination_policy;
  policy.size = sizeof(policy);
  policy.version = FLOWIE_MQTT_CLIENT_DESTINATION_VERSION;
  endpoint.endpoint_id = 1u;
  endpoint.weight = 1u;
  input.size = sizeof(input);
  input.version = CNET_OWNER_PLACEMENT_VERSION;
  input.kind = CNET_OWNER_PLACE_LOWEST_PRESSURE;
  input.owners = hints;
  input.owner_count = 2u;
  if (policy.size == 0u || policy.version == 0u ||
      endpoint.weight != 1u || set_destination == nullptr) return 1;
  if (set_destination(nullptr, nullptr) != SALTS_EINVAL) return 2;
  if (cnet_owner_placement_choose(&input, &owner) != SALTS_OK || owner != 1u) return 3;

  flowie_pattern_selector_t selector{};
  flowie_pattern_selection_iterator_t iterator = FLOWIE_PATTERN_SELECTION_ITERATOR_INIT;
  flowie_pattern_exchange_t exchange{};
  flowie_pattern_exchange_state_t state = FLOWIE_PATTERN_EXCHANGE_READY;
  uint64_t correlation = 0u;
  size_t candidate = SIZE_MAX;
  if (flowie_pattern_selector_init(&selector) != SALTS_OK ||
      flowie_pattern_selection_begin(&selector, FLOWIE_PATTERN_SELECT_ROUND_ROBIN,
                                     3u, &iterator) != SALTS_OK ||
      flowie_pattern_selection_next(&iterator, &candidate) != SALTS_OK ||
      candidate >= 3u)
    return 4;
  if (flowie_pattern_exchange_init(&exchange) != SALTS_OK ||
      flowie_pattern_exchange_begin(&exchange, FLOWIE_PATTERN_EXCHANGE_WAIT_REPLY,
                                    UINT64_C(314159)) != SALTS_OK ||
      flowie_pattern_exchange_snapshot(&exchange, &state, &correlation) != SALTS_OK ||
      state != FLOWIE_PATTERN_EXCHANGE_WAIT_REPLY || correlation != UINT64_C(314159) ||
      flowie_pattern_exchange_reset(&exchange) != SALTS_OK)
    return 5;
  return 0;
}
