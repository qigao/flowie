# Flowie MQTT Client Resilience Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Flowie MQTT client 增加 MQTT 5 `0x89 Server busy` 处理、可选自动重连和认证失败后的 CONNECT 凭据刷新，同时保持现有 `flowie_mqtt_client_create()` 行为不变。

**Architecture:** `flowie_mqtt_client_create_ex()` 注入独立的 resilience 配置，现有 `create()` 以 `NULL` resilience 委托给它。重连模板、退避状态和凭据替换全部由 DLL worker coroutine 单线程拥有；命令/销毁仅通过现有队列与 `coro_wait_interrupt()` 唤醒该 owner，不建立第二条网络状态推进路径。

**Tech Stack:** C11、Flowie MQTT protocol、CoroNet `coro_wait`、TurboUtils/TinyTest、CMake Presets。

**Spec:** `docs/FLOWIE_CONTROL_JWT_JWKS_AUTH.md`

## Global Constraints

- `flowie_mqtt_client_create()` 的参数校验、回调次数和禁用自动重连的既有行为不变。
- MQTT 5 CONNECT 安全提供者返回 `TURBO_EBUSY` 时发送 CONNACK `0x89`；MQTT 3.1.1 保持返回码 `0x03`。
- CONNACK `0x88`/`0x89` 和瞬态网络错误复用最近一次 CONNECT；`0x86`/`0x87` 只有刷新回调成功后才重试。
- MQTT 5 DISCONNECT `0x87` 触发刷新，`0x89` 触发普通重连；`0x8e`、协议错误和永久认证拒绝不重试。
- 所有 span 在 API/回调边界内深拷贝；缓存的 CONNECT password/token 在替换和销毁时清零。
- 退避等待必须使用可中断的 `coro_wait_for()`，不得阻塞 event loop，销毁必须能立即打断等待。
- 不修改、不暂存用户已有的 `server/flowie_worker_runtime.c` 改动。

---

### Task 1: Server busy reason mapping

**Files:**
- Modify: `flowie/src/flowie_endpoint.c`
- Test: `flowie/tests/test_flowie_transport_baseline.c`

**Interfaces:**
- Consumes: HTTP security provider 已有的 `TURBO_EBUSY` 返回值。
- Produces: MQTT 5 CONNACK reason `0x89`；MQTT 3.1.1 return code `0x03`。

- [ ] **Step 1: 扩展真实 endpoint 失败矩阵**

  在 `flowie_transport_auth_unavailable_case()` 增加手工派生的 wire 期望值：

  ```c
  static const uint8_t connack_busy_v5[] = {0x20u, 0x03u, 0x00u, 0x89u, 0x00u};
  {FLOWIE_MQTT_VERSION_5, TURBO_EBUSY, connack_busy_v5, sizeof(connack_busy_v5)},
  {FLOWIE_MQTT_VERSION_3_1_1, TURBO_EBUSY, connack_v311, sizeof(connack_v311)},
  ```

- [ ] **Step 2: 运行 RED**

  Run: `cmake --build --preset win-release-user --target test_flowie_transport_baseline`，随后 `ctest --preset win-release-user -R ^test_flowie_transport_baseline$ --output-on-failure`。

  Expected: 新 MQTT 5 case 收不到 `20 03 00 89 00`，证明 endpoint 尚未映射 `TURBO_EBUSY`。

- [ ] **Step 3: 实现最小错误映射**

  在 CONNECT security boundary 将 `TURBO_EBUSY` 单独映射为命名常量 `FLOWIE_MQTT_REASON_SERVER_BUSY`；不得把协议错误或永久拒绝归入该分支。

- [ ] **Step 4: 运行 GREEN**

  Run: 与 Step 2 相同。

  Expected: target 构建成功，CTest 该测试 1/1 通过。

### Task 2: Public resilience contract and automatic retry

**Files:**
- Modify: `flowie/client/include/flowie_mqtt_client.h`
- Modify: `flowie/client/src/flowie_mqtt_client.c`
- Test: `flowie/client/tests/test_flowie_mqtt_client.c`

**Interfaces:**
- Consumes: 最近一次成功提交的 `flowie_mqtt_connect_packet_t` 深拷贝。
- Produces:

  ```c
  typedef int (*flowie_mqtt_client_refresh_connect_fn)(
      flowie_mqtt_client_t *client, uint8_t reason_code,
      const flowie_mqtt_connect_packet_t *current,
      flowie_mqtt_connect_packet_t *refreshed, void *user_data);

  typedef void (*flowie_mqtt_client_reconnect_fn)(
      flowie_mqtt_client_t *client, uint32_t attempt, int status,
      const flowie_mqtt_control_packet_view_t *response, void *user_data);

  typedef struct flowie_mqtt_client_resilience_config_s {
    size_t size;
    uint64_t initial_delay_ms;
    uint64_t max_delay_ms;
    uint32_t max_attempts; /* 0 means unlimited. */
    flowie_mqtt_client_refresh_connect_fn refresh_connect;
    flowie_mqtt_client_reconnect_fn on_reconnect;
  } flowie_mqtt_client_resilience_config_t;

  FLOWIE_MQTT_CLIENT_C_API int flowie_mqtt_client_create_ex(
      const flowie_mqtt_client_config_t *config,
      const flowie_mqtt_client_resilience_config_t *resilience,
      flowie_mqtt_client_t **out);
  ```

- [ ] **Step 1: 写真实 broker RED tests**

  添加一个本地 CoroNet broker fixture：第一次 CONNECT 返回 literal CONNACK `0x89`，第二次返回成功。断言公开 `on_connect` 仍只完成原命令一次，`on_reconnect` 收到 attempt `1` 和成功 CONNACK，broker 恰好收到两次 CONNECT。

- [ ] **Step 2: 运行 RED**

  Run: `cmake --build --preset win-release-user --target test_flowie_mqtt_client`。

  Expected: 因 `flowie_mqtt_client_create_ex` 与 resilience 类型不存在而编译失败。

- [ ] **Step 3: 实现配置验证与 owner 状态**

  `create()` 委托 `create_ex(config, NULL, out)`；`create_ex()` 校验独立结构体的精确 `size`、退避范围和溢出，创建一个复用 `coro_wait_t`。client opaque struct 持有深拷贝 CONNECT、attempt、delay/deadline，所有字段只由 worker pump 修改。

- [ ] **Step 4: 实现 0x88/0x89 与网络错误重连**

  初始命令仍调用 `on_connect` 一次。之后仅对 CONNACK `0x88`/`0x89` 或 `TURBO_EOF`、`TURBO_ECONNRESET`、`TURBO_ECONNREFUSED`、`TURBO_ETIMEDOUT`、`TURBO_ENETDOWN`、`TURBO_ENETUNREACH`、`TURBO_EHOSTUNREACH`、`TURBO_EIO` 安排指数退避；每次内部 attempt 通过 `on_reconnect` 报告，成功后重置 attempt/delay。

- [ ] **Step 5: 运行 GREEN**

  Run: build target 后直接运行 `build\Msvc-Release\bin\test_flowie_mqtt_client.exe --filter "reconnect"`。

  Expected: server-busy 自动重连测试通过。

### Task 3: Token refresh, DISCONNECT classification, and prompt shutdown

**Files:**
- Modify: `flowie/client/src/flowie_mqtt_client.c`
- Test: `flowie/client/tests/test_flowie_mqtt_client.c`

**Interfaces:**
- Consumes: Task 2 的 `refresh_connect` 与 retained CONNECT。
- Produces: `0x86`/`0x87` 刷新后重连；DISCONNECT `0x87`/`0x89` 分类；可中断退避。

- [ ] **Step 1: 写 token replacement RED test**

  broker 第一次解析到 password literal `token-1` 后返回 `0x87`；刷新回调从 borrowed current 复制结构并把 password 改为 literal `token-2`；第二次 CONNECT 必须实际携带 `token-2` 后 broker 才返回成功。

- [ ] **Step 2: 写 connected DISCONNECT RED tests**

  成功连接后 broker 发送 `E0 02 87 00`，断言刷新并重连；发送 `E0 02 8E 00` 时断言没有第二次 CONNECT。两个期望值均由测试 literal 提供，不复用生产分类 helper。

- [ ] **Step 3: 写 shutdown RED test**

  配置长退避，等待第一个失败 attempt 进入退避后调用 destroy；断言销毁耗时低于测试上限且不会在销毁后调用回调。

- [ ] **Step 4: 实现刷新和原因保留**

  `handle_unsolicited()` 解析并保留 DISCONNECT reason，而不是全部折叠成无原因的 `TURBO_ECONNRESET`。刷新回调只在 worker coroutine 调用；返回成功后立即深拷贝 replacement，再清零并释放旧 template。

- [ ] **Step 5: 实现可中断退避**

  pump 使用 `coro_wait_for()` 等待剩余 deadline；命令提交或销毁通过现有 `coro_post()` 进入 worker wake，再以 `coro_wait_interrupt(..., TURBO_EINTR/TURBO_ESHUTDOWN)` 恢复 owner coroutine。`0x8e` 和 `TURBO_EPROTO` 清除 pending retry。

- [ ] **Step 6: 运行 GREEN 与 mutation checks**

  Run: `test_flowie_mqtt_client.exe --filter "refresh"`、`--filter "session taken over"`、`--filter "backoff"`。

  Expected: 三组均通过；若删除 token replacement、DISCONNECT reason 保存、wait interrupt 或 `0x8e` 排除分支，相应测试会失败。

### Task 4: Documentation and regression verification

**Files:**
- Modify: `flowie/CLIENT_GUIDE.md`
- Modify: `docs/FLOWIE_CONTROL_JWT_JWKS_AUTH.md`

**Interfaces:**
- Consumes: 最终公开头文件契约和已验证行为。
- Produces: JWT/JWKS 与 MQTT enhanced AUTH 的边界说明、`create_ex()` 示例和 reason-code 策略。

- [ ] **Step 1: 更新文档**

  说明 `AUTH 0x19` 仍是 MQTT enhanced authentication 的显式 re-auth；JWT bearer 位于 CONNECT password 时，`0x86`/`0x87` 通过 refresh callback 生成新的完整 CONNECT。注明回调运行于 worker coroutine，不得执行阻塞/重型函数。

- [ ] **Step 2: 跑最小和相邻回归**

  Run:

  ```text
  cmake --build --preset win-release-user --target test_flowie_mqtt_client test_flowie_transport_baseline test_flowie_security test_flowie_server_http_security
  ctest --preset win-release-user -R "^(test_flowie_mqtt_client|test_flowie_transport_baseline|test_flowie_security|test_flowie_server_http_security)$" --output-on-failure
  git diff --check
  ```

  Expected: 四个 targets 构建成功，四个 CTest 0 failures，diff whitespace 检查通过。

- [ ] **Step 3: 核对变更边界**

  `git status --short` 中仅包含本任务文件与先前 JWT/0x88 文件；`server/flowie_worker_runtime.c` 仍保持用户原有修改且未被本任务编辑。
