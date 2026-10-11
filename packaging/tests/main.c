#include <flowie.h>
#include <flowie_mqtt_client.h>
#include <flowie_mqtt_protocol.h>
#include <cmeta_error.h>

/* Call every exported library so link/load qualification cannot be optimized
 * into a header-only sizeof check. No sockets or external services are needed. */
int main(void) {
  static const uint8_t topic[] = "flowie/release";
  const flowie_mqtt_span_t span = {topic, sizeof(topic) - 1u};
  if (flowie_endpoint_core_stop(NULL) != SALTS_EINVAL) return 1;
  if (flowie_mqtt_client_create(NULL, NULL) != SALTS_EINVAL) return 2;
  if (!flowie_mqtt_utf8_validate(span)) return 3;
  return 0;
}
