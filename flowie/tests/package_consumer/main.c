#include "flowie.h"
#include "flowie_mqtt_client.h"

int main(void) {
  flowie_endpoint_config_t endpoint = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_mqtt_client_config_t client = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  if (endpoint.size != sizeof(endpoint) || client.size != sizeof(client)) return 1;
  if (endpoint.transport != FLOWIE_TRANSPORT_TCP ||
      client.transport != FLOWIE_MQTT_CLIENT_TRANSPORT_TCP) return 1;
  return client.port == FLOWIE_MQTT_CLIENT_DEFAULT_PORT ? 0 : 1;
}
