# Flowie TurboUtils Core Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate Flowie from removed TurboUtils compatibility APIs to component-owned export macros, `tstr/vstr`, and `Rocida::stl` without changing MQTT, cluster, control, or storage behavior.

**Architecture:** Keep Flowie Protocol and Flowie Core as separate DLL ABI owners. Protocol declarations use `FLOWIE_PROTOCOL_C_API`; the main Flowie library and its internal declarations use `FLOWIE_C_API`. String and container changes are mechanical API migrations, with status translation centralized at the Flowie boundary.

**Tech Stack:** C11/C++17, CMake Presets, Rocida::Core, Rocida::STL, TinyTest.

**Spec:** User request in this conversation dated 2026-08-22.

## Global Constraints

- Preserve every pre-existing dirty and untracked Flowie change.
- Do not commit, reset, checkout, stash, or overwrite user work.
- Keep public behavior, protocol bytes, ownership, and error semantics unchanged.
- Use `win-release-user` for Windows configure, build, and CTest verification.
- A clean build failure at `CXX_C_API` is the recorded RED baseline.

---

### Task 1: Component-owned C ABI exports

**Files:**
- Create: `flowie/protocol/include/flowie_protocol_export.h`
- Create: `flowie/include/flowie_export.h`
- Modify: `flowie/protocol/CMakeLists.txt`
- Modify: `flowie/CMakeLists.txt`
- Modify: `cluster/flowie_cluster_internal.h`
- Modify: `cluster/flowie_cluster_peer_internal.h`
- Modify: `flowie/include/flowie.h`
- Modify: `flowie/include/flowie_message.h`
- Modify: `flowie/include/flowie_protocol_contract.h`
- Modify: `flowie/include/flowie_protocol_repository.h`
- Modify: `flowie/include/flowie_security.h`
- Modify: `flowie/protocol/include/flowie_mqtt_protocol.h`
- Modify: `flowie/src/flowie_bitmap_index_internal.h`
- Modify: `flowie/src/flowie_ingress_internal.h`
- Modify: `flowie/src/flowie_rule_internal.h`
- Modify: `flowie/src/flowie_security_internal.h`
- Modify: `flowie/src/flowie_session_internal.h`
- Modify: `flowie/src/flowie_topic_index_internal.h`

**Interfaces:**
- Produces: `FLOWIE_PROTOCOL_C_API` for Protocol DLL declarations.
- Produces: `FLOWIE_C_API` for the main Flowie DLL and internal C-linkage declarations.

- [ ] **Step 1: Verify RED**

Run: `cmake --fresh --preset win-release-user && cmake --build --preset win-release-user`

Expected: compilation fails because `CXX_C_API` is undefined.

- [ ] **Step 2: Add export headers**

```c
#ifdef __cplusplus
#define FLOWIE_C_API extern "C" FLOWIE_API
#else
#define FLOWIE_C_API FLOWIE_API
#endif
```

Use the equivalent `FLOWIE_PROTOCOL_C_API` boundary for the Protocol DLL.

- [ ] **Step 3: Replace declarations and configure DLL definitions**

Protocol declarations consume `FLOWIE_PROTOCOL_C_API`; Flowie declarations consume `FLOWIE_C_API`. Test plugin exports use plain platform-specific C export declarations because the installed TurboFlow package still exposes the removed compatibility macro.

- [ ] **Step 4: Build the smallest target**

Run: `cmake --build --preset win-release-user --target flowie_protocol`

Expected: Protocol target compiles or advances to the next removed TurboUtils API.

### Task 2: String/view migration

**Files:**
- Modify: `cluster/flowie_cluster_broadcast_dispatch.c`
- Modify: `cluster/flowie_cluster_broadcast_event.c`
- Modify: `cluster/flowie_cluster_broadcast_event_internal.h`
- Modify: `cluster/flowie_cluster_broadcast_target_dispatch.c`
- Modify: `cluster/flowie_cluster_broadcast_target_dispatch_internal.h`
- Modify: `cluster/flowie_cluster_broadcast_target_owner.c`
- Modify: `cluster/flowie_cluster_delivery_action.c`
- Modify: `cluster/flowie_cluster_delivery_action_internal.h`
- Modify: `cluster/flowie_cluster_delivery_dispatch.c`
- Modify: `cluster/flowie_cluster_delivery_dispatch_internal.h`
- Modify: `cluster/flowie_cluster_edge_bind.c`
- Modify: `cluster/flowie_cluster_edge_bind_internal.h`
- Modify: `cluster/flowie_cluster_endpoint_binding.c`
- Modify: `cluster/flowie_cluster_endpoint_binding_internal.h`
- Modify: `cluster/flowie_cluster_generation.c`
- Modify: `cluster/flowie_cluster_lifecycle_owner.c`
- Modify: `cluster/flowie_cluster_member_directory.c`
- Modify: `cluster/flowie_cluster_member_directory_internal.h`
- Modify: `cluster/flowie_cluster_membership_controller.c`
- Modify: `cluster/flowie_cluster_membership_runtime.c`
- Modify: `cluster/flowie_cluster_node.c`
- Modify: `cluster/flowie_cluster_node_router.c`
- Modify: `cluster/flowie_cluster_node_router_internal.h`
- Modify: `cluster/flowie_cluster_owner_directory.c`
- Modify: `cluster/flowie_cluster_owner_directory_internal.h`
- Modify: `cluster/flowie_cluster_owner_directory_runtime.c`
- Modify: `cluster/flowie_cluster_peer_authority.c`
- Modify: `cluster/flowie_cluster_peer_authority_internal.h`
- Modify: `cluster/flowie_cluster_peer_connect_bind.c`
- Modify: `cluster/flowie_cluster_peer_connection_lost.c`
- Modify: `cluster/flowie_cluster_peer_connector.c`
- Modify: `cluster/flowie_cluster_peer_connector_internal.h`
- Modify: `cluster/flowie_cluster_peer_edge_action.c`
- Modify: `cluster/flowie_cluster_peer_internal.h`
- Modify: `cluster/flowie_cluster_peer_listener.c`
- Modify: `cluster/flowie_cluster_peer_listener_internal.h`
- Modify: `cluster/flowie_cluster_peer_mqtt_command.c`
- Modify: `cluster/flowie_cluster_peer_mqtt_reply.c`
- Modify: `cluster/flowie_cluster_peer_owner.c`
- Modify: `cluster/flowie_cluster_peer_protocol.c`
- Modify: `cluster/flowie_cluster_peer_publish_settle.c`
- Modify: `cluster/flowie_cluster_peer_registry.c`
- Modify: `cluster/flowie_cluster_peer_takeover_close.c`
- Modify: `cluster/flowie_cluster_peer_transport.c`
- Modify: `cluster/flowie_cluster_pgsql.c`
- Modify: `cluster/flowie_cluster_pgsql_fact.c`
- Modify: `cluster/flowie_cluster_pgsql_fact_worker.c`
- Modify: `cluster/flowie_cluster_pgsql_internal.h`
- Modify: `cluster/flowie_cluster_pgsql_worker.c`
- Modify: `cluster/flowie_cluster_publish_egress.c`
- Modify: `cluster/flowie_cluster_publish_egress_internal.h`
- Modify: `cluster/flowie_cluster_publish_event.c`
- Modify: `cluster/flowie_cluster_publish_event_internal.h`
- Modify: `cluster/flowie_cluster_publish_router.c`
- Modify: `cluster/flowie_cluster_publish_router_internal.h`
- Modify: `cluster/flowie_cluster_raft_generation.c`
- Modify: `cluster/flowie_cluster_raft_generation_internal.h`
- Modify: `cluster/flowie_cluster_redis_bus.c`
- Modify: `cluster/flowie_cluster_redis_bus_internal.h`
- Modify: `cluster/flowie_cluster_route_projection.c`
- Modify: `cluster/flowie_cluster_route_projection_internal.h`
- Modify: `cluster/flowie_cluster_route_reconcile.c`
- Modify: `cluster/flowie_cluster_route_store_internal.h`
- Modify: `cluster/flowie_cluster_route_store_orm.c`
- Modify: `cluster/flowie_cluster_session_bind.c`
- Modify: `cluster/flowie_cluster_session_bind_internal.h`
- Modify: `cluster/flowie_cluster_shard_runtime.c`
- Modify: `cluster/flowie_cluster_takeover_dispatch.c`
- Modify: `cluster/flowie_cluster_takeover_dispatch_internal.h`
- Modify: `cluster/flowie_cluster_topology.c`
- Modify: `cluster/flowie_cluster_topology_internal.h`
- Modify: `cluster/tests/test_flowie_cluster_broadcast_dispatch.c`
- Modify: `cluster/tests/test_flowie_cluster_broadcast_event.c`
- Modify: `cluster/tests/test_flowie_cluster_broadcast_target_dispatch.c`
- Modify: `cluster/tests/test_flowie_cluster_broadcast_target_owner.c`
- Modify: `cluster/tests/test_flowie_cluster_delivery_action.c`
- Modify: `cluster/tests/test_flowie_cluster_delivery_dispatch.c`
- Modify: `cluster/tests/test_flowie_cluster_edge_bind.c`
- Modify: `cluster/tests/test_flowie_cluster_generation.c`
- Modify: `cluster/tests/test_flowie_cluster_lifecycle_dispatch.c`
- Modify: `cluster/tests/test_flowie_cluster_lifecycle_owner.c`
- Modify: `cluster/tests/test_flowie_cluster_member_directory.c`
- Modify: `cluster/tests/test_flowie_cluster_membership_controller.c`
- Modify: `cluster/tests/test_flowie_cluster_membership_runtime.c`
- Modify: `cluster/tests/test_flowie_cluster_node.c`
- Modify: `cluster/tests/test_flowie_cluster_node_router.c`
- Modify: `cluster/tests/test_flowie_cluster_owner_directory.c`
- Modify: `cluster/tests/test_flowie_cluster_owner_projection.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_authority.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_connect_bind.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_connection_lost.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_connector.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_edge_action.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_listener.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_mqtt_command.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_mqtt_reply.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_owner.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_protocol.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_publish_settle.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_registry.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_takeover_close.c`
- Modify: `cluster/tests/test_flowie_cluster_peer_transport.c`
- Modify: `cluster/tests/test_flowie_cluster_pgsql_live.c`
- Modify: `cluster/tests/test_flowie_cluster_publish_egress.c`
- Modify: `cluster/tests/test_flowie_cluster_publish_event.c`
- Modify: `cluster/tests/test_flowie_cluster_publish_ingress.c`
- Modify: `cluster/tests/test_flowie_cluster_publish_router.c`
- Modify: `cluster/tests/test_flowie_cluster_publish_stream.c`
- Modify: `cluster/tests/test_flowie_cluster_raft_generation.c`
- Modify: `cluster/tests/test_flowie_cluster_redis_bus.c`
- Modify: `cluster/tests/test_flowie_cluster_route_projection.c`
- Modify: `cluster/tests/test_flowie_cluster_route_reconcile_orm.c`
- Modify: `cluster/tests/test_flowie_cluster_route_store_orm.c`
- Modify: `cluster/tests/test_flowie_cluster_session_bind.c`
- Modify: `cluster/tests/test_flowie_cluster_state_machine.c`
- Modify: `cluster/tests/test_flowie_cluster_takeover_dispatch.c`
- Modify: `cluster/tests/test_flowie_cluster_topology.c`
- Modify: `control/flowie_control_dashboard_view.c`
- Modify: `control/flowie_control_external_https_authenticator.c`
- Modify: `control/flowie_control_pgsql_command.c`
- Modify: `control/flowie_control_pgsql_database.c`
- Modify: `control/flowie_control_pgsql_query.c`
- Modify: `control/flowie_control_store.c`
- Modify: `control/flowie_security_sqlite.c`
- Modify: `flowie/benchmarks/bench_flowie.c`
- Modify: `flowie/client/src/flowie_mqtt_client.c`
- Modify: `flowie/include/flowie_message.h`
- Modify: `flowie/protocol/parser/flowie_mqtt_lexer.re`
- Modify: `flowie/src/flowie.c`
- Modify: `flowie/src/flowie_client_adapter.c`
- Modify: `flowie/src/flowie_endpoint.c`
- Modify: `flowie/src/flowie_ingress.c`
- Modify: `flowie/src/flowie_proxy_protocol_internal.h`
- Modify: `flowie/src/flowie_rule.c`
- Modify: `flowie/src/flowie_security_internal.h`
- Modify: `flowie/src/flowie_session.c`
- Modify: `flowie/src/flowie_topic_index.c`
- Modify: `flowie/src/flowie_topic_index_internal.h`
- Modify: `flowie/tests/test_flowie.c`
- Modify: `flowie/tests/test_flowie_endpoint.c`
- Modify: `flowie/tests/test_flowie_ingress.c`
- Modify: `server/flowie_supervisor_runtime.c`

**Interfaces:**
- Consumes: installed `turbo_str.h` and `turbo_vstr.h`.
- Produces: owned `tstr` and borrowed `vstr` usage with unchanged lifetime rules.

- [ ] **Step 1: Capture the first string compile failure**

Run: `cmake --build --preset win-release-user`

Expected: failure names an obsolete `tstr_t`, `tstr_v`, or `tstr_v_*` symbol.

- [ ] **Step 2: Apply the mechanical type/function mapping**

```text
tstr_t -> tstr
tstr_v -> vstr
tstr_v_* -> vstr_*
TSTR_NULL -> NULL
```

- [ ] **Step 3: Rebuild**

Run: `cmake --build --preset win-release-user`

Expected: string compatibility failures are absent.

### Task 3: TurboSTL container migration

**Files:**
- Modify: `cluster/flowie_cluster_broadcast_target_owner.c`
- Modify: `cluster/flowie_cluster_edge_bind.c`
- Modify: `cluster/flowie_cluster_endpoint_binding.c`
- Modify: `cluster/flowie_cluster_node.c`
- Modify: `cluster/flowie_cluster_owner_directory.c`
- Modify: `cluster/flowie_cluster_peer_authority.c`
- Modify: `cluster/flowie_cluster_peer_transport.c`
- Modify: `cluster/flowie_cluster_pgsql_fact_worker.c`
- Modify: `cluster/flowie_cluster_publish_egress.c`
- Modify: `cluster/flowie_cluster_publish_ingress.c`
- Modify: `cluster/flowie_cluster_publish_router.c`
- Modify: `cluster/flowie_cluster_redis_bus.c`
- Modify: `cluster/flowie_cluster_session_bind.c`
- Modify: `cluster/flowie_cluster_topology.c`
- Modify: `control/flowie_control_auth_cache.c`
- Modify: `control/flowie_control_auth_rate_limiter.c`
- Modify: `control/flowie_control_management_session.c`
- Modify: `control/flowie_control_principal_cache.c`
- Modify: `flowie/benchmarks/bench_flowie.c`
- Modify: `flowie/client/src/flowie_mqtt_client.c`
- Modify: `flowie/src/flowie_endpoint.c`
- Modify: `flowie/src/flowie_proxy_protocol.c`
- Modify: `flowie/src/flowie_session.c`
- Modify: `flowie/src/flowie_topic_index.c`
- Modify: `flowie/src/flowie_topic_index_internal.h`
- Modify: `flowie/tests/test_flowie_mqtt_soak.c`
- Modify: `flowie/tests/test_flowie_topic_index.c`
- Create: `flowie/include/flowie_stl_error_internal.h`
- Modify: `flowie/CMakeLists.txt`
- Modify: `cluster/CMakeLists.txt`
- Modify: `control/CMakeLists.txt`

**Interfaces:**
- Consumes: the direct `<turbostl/*.h>` APIs and `Rocida::STL`.
- Produces: only `flowie_stl_error(stl_status)` at Flowie's error-domain boundary; it does not proxy container operations.

- [ ] **Step 1: Capture the first container compile failure**

Run: `cmake --build --preset win-release-user`

Expected: failure names an old container header, init signature, or status type.

- [ ] **Step 2: Migrate headers, typed wrappers, and initialization**

Use direct `*_init_bytes` calls for Flowie's container element types because the current CMeta
universe does not describe the required fixed-width integers and pointer types. Preserve configured
capacity limits and translate status only where it crosses into Flowie's `TURBO_E*` error domain.

- [ ] **Step 3: Add private STL linkage**

Link only targets that compile container implementation code to `Rocida::STL`; do not expose container implementation types in installed APIs.

- [ ] **Step 4: Rebuild**

Run: `cmake --build --preset win-release-user`

Expected: container compatibility failures are absent.

### Task 4: Adjacent compatibility failures

**Files:**
- Modify only files named by fresh compiler/test output.

**Interfaces:**
- Consumes: current TinyTest generic assertions and current TLOG typed formatting macros.
- Produces: unchanged assertions and log messages under current APIs.

- [ ] **Step 1: Run build and record each new first failure**

Run: `cmake --build --preset win-release-user`

- [ ] **Step 2: Apply the minimum API-compatible correction**

Migrate removed assertion/logging spellings only when the compiler identifies them; do not refactor behavior.

- [ ] **Step 3: Rebuild until compilation completes**

Run: `cmake --build --preset win-release-user`

Expected: exit code 0.

### Task 5: Verification and audit

**Files:**
- Verify all modified files from Tasks 1–4.

**Interfaces:**
- Produces: reproducible evidence for build, tests, legacy-symbol removal, and preservation of user changes.

- [ ] **Step 1: Run full tests**

Run: `ctest --preset win-release-user --output-on-failure`

Expected: all registered tests pass.

- [ ] **Step 2: Scan removed APIs**

Run: `rg.exe -n --glob '!build/**' --glob '!AGENTS.md' "CXX_C_API|\\btstr_t\\b|\\btstr_v\\b|tstr_v_|turbo_(vec|hash_map|hash_set|set|deque|heap)\\.h" .`

Expected: no Flowie-owned production/test matches; external installed TurboFlow findings are reported separately.

- [ ] **Step 3: Validate patch hygiene**

Run: `git diff --check`

Expected: no whitespace errors.

- [ ] **Step 4: Report preservation and residual risks**

Compare final status with the recorded dirty baseline, distinguishing pre-existing changes from this migration. Do not commit.
