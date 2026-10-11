#ifndef FLOWIE_MQTT_CLIENT_FAULT_TEST_H
#define FLOWIE_MQTT_CLIENT_FAULT_TEST_H

/* Private declarations for the isolated fault-injected Client DLL.
 * Never install this file or enable FLOWIE_CLIENT_FAULT_TEST for Flowie::Client. */
#include "flowie_mqtt_client.h"
#include <cnet/cnet.h>

/* Read-only admission diagnostics and pure protocol gate; no packet injection. */
FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_receive_admissions(const flowie_mqtt_client_t *client);
FLOWIE_MQTT_CLIENT_C_API int
flowie_mqtt_client_test_connect_packet_admit(flowie_mqtt_packet_type_t type);

/* Inspect the actual Managed Dial admission on its owner, then return an error
 * to stop before network I/O. Install before CONNECT; context lives through
 * Client destruction. This hook is absent from the installed Client ABI. */
typedef int (*flowie_mqtt_client_test_dial_fn)(const cnet_connect_options *, void *);
FLOWIE_MQTT_CLIENT_C_API int
flowie_mqtt_client_test_set_dial_admission(flowie_mqtt_client_t *client,
    flowie_mqtt_client_test_dial_fn inspect, void *context);

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

/* Test-only WS/WSS destruction timeout; real CHttp context remains owned
 * until the original Worker successfully calls native destroy. */
FLOWIE_MQTT_CLIENT_C_API int
flowie_mqtt_client_test_set_ws_destroy_timeout(flowie_mqtt_client_t *client, int enabled);
FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_ws_destroy_timeout_hits(const flowie_mqtt_client_t *client);

#endif
