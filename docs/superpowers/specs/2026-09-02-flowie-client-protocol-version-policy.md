# Flowie Client Protocol Version Policy

## Decision

Flowie Client uses MQTT 5 as the default protocol version. Callers that need MQTT 3 select
`FLOWIE_MQTT_VERSION_3_1_1` (preferred legacy version) or `FLOWIE_MQTT_VERSION_3_1` through the new
`flowie_mqtt_client_set_version()` API before the first versioned command is accepted.

Packet descriptions keep their existing `version` fields. `FLOWIE_MQTT_VERSION_UNSPECIFIED` means
"inherit the client's selected version". An explicit packet version must equal the selected client
version; a mismatch fails immediately with `TURBO_EPROTO` and is never admitted to the worker queue.

## Background

The current client stores only the active connection version. It starts and returns to
`FLOWIE_MQTT_VERSION_UNSPECIFIED`, while every CONNECT, PUBLISH, SUBSCRIBE, and UNSUBSCRIBE caller
must repeat a concrete version. That makes the packet descriptions the effective source of truth
and leaves no client-level default or stable reconnect policy.

MQTT 5 and MQTT 3.1.1 use different wire layouts and semantics. MQTT 5 adds properties and reason
codes; MQTT 3 packets must not silently inherit those fields. The protocol encoder remains the
authoritative validator for packet-specific rules.

Official protocol references:

- MQTT 5.0: <https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html>
- MQTT 3.1.1: <https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html>

## State ownership

The client owns two distinct values:

- `selected_version`: the stable policy value, initialized to MQTT 5 and protected by
  `command_mutex` until the first versioned command is admitted.
- `version`: the active connection value, owned by the worker coroutine and reset to
  `UNSPECIFIED` when the transport closes.

The first successfully admitted CONNECT, PUBLISH, SUBSCRIBE, or UNSUBSCRIBE locks
`selected_version`. Failed validation, allocation, queue-capacity checks, or `coro_post()` do not
lock it. Reconnect CONNECT packets already carry the resolved concrete version and cannot change
the selected protocol.

## Public API

```c
int flowie_mqtt_client_set_version(flowie_mqtt_client_t *client,
                                   flowie_mqtt_version_t version);
```

The function accepts MQTT 3.1, MQTT 3.1.1, or MQTT 5. It returns:

- `TURBO_OK` when the selected version is updated;
- `TURBO_EINVAL` for a null client, `UNSPECIFIED`, or an unsupported value;
- `TURBO_EALREADY` after a versioned command has been accepted;
- `TURBO_ESHUTDOWN` after shutdown begins.

It performs no I/O and invokes no callback.

## Compatibility and migration

The opaque client layout may grow without changing the public struct ABI. No existing public
configuration or packet structure changes size. Adding one exported C function is binary-compatible
for existing consumers.

Source behavior changes intentionally for MQTT 3 consumers: before submitting any packet they must
call `flowie_mqtt_client_set_version(client, FLOWIE_MQTT_VERSION_3_1_1)` (or explicit MQTT 3.1).
Existing MQTT 5 callers remain compatible. Callers may then omit all packet `version` assignments;
existing explicit MQTT 5 assignments remain valid.

Rollback consists of removing the new function and selected-version state, restoring mandatory
per-packet versions. No persisted data, wire format, deployment configuration, or dependency changes
are involved.

## Validation scope

Wire-level local broker tests must prove:

- unspecified packet versions encode as MQTT 5 by default;
- selecting MQTT 3.1.1 or 3.1 makes unspecified packets encode with that exact version;
- explicit matching versions still work;
- unsupported selections, post-admission selection, and explicit mismatches fail with the documented
  error without silent fallback;
- the existing MQTT 5 capability, reason-code, properties, enhanced-authentication, reconnect, and
  MQTT 3 callback suites remain green.
