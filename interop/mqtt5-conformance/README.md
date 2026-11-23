# MQTT 5 broker conformance

Use [mqtt-lib](https://github.com/LabOverWire/mqtt-lib/tree/1412692e6715d0a01a405180f8682ba0daf3d77e)
at `1412692e6715d0a01a405180f8682ba0daf3d77e`. Build the CLI from that checkout with the suite's
`conformance` profile.

```text
cargo build --locked --profile conformance -p mqtt5-conformance-cli
```

For positive Topic Alias tests, set `topic_alias_maximum: 10` on the Flowie listener and
`topic_alias_maximum = 10` in the SUT descriptor. Use dedicated anonymous loopback TCP and
WebSocket listeners and declare the other capabilities as advertised by the test endpoints.
For standalone Flowie, set `sys_topics_published = true` and `dollar_sys_publish = true` in
`[capabilities]` on the anonymous loopback fixture. The suite reads the retained
`$SYS/broker/version` and publishes to a separate `$SYS/conformance/` topic. On authenticated
fixtures, enable `dollar_sys_publish` only when that test identity can publish to its test topic.

The stock suite uses an unsupported protocol level to trigger three error CONNACK assertions.
MQTT 5 permits closing that connection directly. To exercise the three assertions against
Flowie's anonymous listener, apply [error-connack.patch](error-connack.patch) to the pinned
mqtt-lib checkout with `git apply` and rebuild the CLI. The patch uses an unsupported
Authentication Method in a valid MQTT 5 CONNECT and expects `0x8C`. The tests still check
Session Present, the reason code and connection closure. The separate unsupported protocol
version test remains part of the run.

Run `mqtt5-conformance --sut <descriptor.toml> --report <results.json>`. Keep the stock and
adapted reports so the change in test fixture is visible. MQTT 5 defines the optional
unsupported-version response in section 3.1.2.2 and the CONNACK requirements in section 3.2.2.
[MQTT 5.0 OASIS standard](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html).
