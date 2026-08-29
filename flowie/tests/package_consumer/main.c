#include "flowie.h"
#include "flowie_mqtt_client.h"
#include "flowie_protocol_repository.h"

int main(void) {
  flowie_endpoint_config_t endpoint = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_mqtt_client_config_t client = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  orm_config_t database = {0};
  flowie_protocol_repository_config_t repository = FLOWIE_PROTOCOL_REPOSITORY_CONFIG_INIT;
  repository.database = &database;
  if (endpoint.size != sizeof(endpoint) || client.size != sizeof(client)) return 1;
  if (repository.size != sizeof(repository) || repository.database != &database) return 1;
  if (endpoint.transport != FLOWIE_TRANSPORT_TCP ||
      client.transport != FLOWIE_MQTT_CLIENT_TRANSPORT_TCP) return 1;
  return client.port == FLOWIE_MQTT_CLIENT_DEFAULT_PORT ? 0 : 1;
}
