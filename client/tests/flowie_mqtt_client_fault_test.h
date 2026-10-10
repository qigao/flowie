#ifndef FLOWIE_MQTT_CLIENT_FAULT_TEST_H
#define FLOWIE_MQTT_CLIENT_FAULT_TEST_H

/* Private declarations for the isolated fault-injected Client DLL.
 * Never install this file or enable FLOWIE_CLIENT_FAULT_TEST for Flowie::Client. */
#include "flowie_mqtt_client.h"

FLOWIE_MQTT_CLIENT_C_API int
flowie_mqtt_client_test_force_close_full(flowie_mqtt_client_t *client, int enabled);
FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_close_full_hits(const flowie_mqtt_client_t *client);
FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_close_full_progress(const flowie_mqtt_client_t *client);
FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_close_full_wrong_owner(const flowie_mqtt_client_t *client);

/* Test-only CNet stop-boundary fault modes; never install this header.
 * Mode 1 returns a synthetic callback error after a REAL native stop.
 * Mode 2 reports a timeout before native stop; Client must be retained. */
FLOWIE_MQTT_CLIENT_C_API int
flowie_mqtt_client_test_set_native_stop_mode(flowie_mqtt_client_t *client, int mode);
FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_native_stop_timeout_hits(const flowie_mqtt_client_t *client);
FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_native_stop_reported_errors(void);

#endif
