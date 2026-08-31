# Flowie 原生远程 Auth/ACL 执行修复 Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 让当前 `flowie_server` 真正读取既有 EU/SH `flowie.yml` 的 endpoint、HTTPS Auth、HTTPS ACL 与 security realm 配置，并在 MQTT 数据面执行拒绝；Flowie 默认不依赖 RulesForge、TurboFlow、FlowMQ 或 TurboRaft。

**Architecture:** `flowie_server` 是 standalone composition root。YAML 是 endpoint/security 配置的唯一事实源；一个 Flowie 原生 HTTPS provider 同时实现认证与逐请求授权接口，并通过 `TurboHttp::TurboHttp` 调用 flowie-control。Endpoint 直接回送已准入的 MQTT PUBLISH，现有 `.flow` 的 direct source-to-sink 拓扑不需要规则引擎执行。安全配置存在或 `--require-security` 启用时，缺字段、TLS/HTTP 失败与非法响应全部 fail closed。

**Tech Stack:** C11、Flowie MQTT endpoint/security、TurboParser YAML DOM、TurboHTTP facade、TinyTest、CMake Presets。

---

### Task 1: 固化 Flowie 授权边界

**Files:**
- Modify: `flowie/tests/CMakeLists.txt`
- Create: `flowie/tests/test_flowie_security.c`
- Modify: `flowie/src/flowie_security.c`

- [x] 远程 provider 返回 `TURBO_OK + DENY` 时，协议授权边界返回 `TURBO_EPERM`。
- [x] 静态规则默认拒绝也返回 `TURBO_EPERM`，显式允许保持成功。
- [x] 聚焦测试已先失败再通过。

### Task 2: 原生解析既有 YAML

**Files:**
- Create: `server/flowie_server_config.c`
- Create: `server/flowie_server_config_internal.h`
- Create: `server/tests/test_flowie_server_config.c`
- Modify: `server/CMakeLists.txt`

- [x] 用真实 EU fixture 固定 profile -> endpoint/auth provider/security realm/ACL provider 引用解析。
- [x] 读取 endpoint 有界参数以及 HTTPS URL、service identity、env secret ref、timeout 和 CA 文件。
- [x] 严格拒绝缺失引用、非 HTTPS backend、未配置 CA、未知 secure transport 或不完整安全配置。

### Task 3: Flowie 原生 TurboHTTP Auth/ACL provider

**Files:**
- Create: `server/flowie_server_http_security.c`
- Create: `server/flowie_server_http_security_internal.h`
- Create: `server/tests/test_flowie_server_http_security.c`
- Modify: `server/CMakeLists.txt`

- [x] 用测试固定 Auth v3 与 ACL v4 请求/响应协议转换。
- [x] 使用 `TurboHttp::TurboHttp` facade；TLS 始终校验证书，禁止 redirect/retry，响应有界。
- [x] token 仅从 `env://` 引用读取，不写入日志，临时 secret/header/body 在释放前擦除。
- [x] provider/realm 生命周期长于 endpoint；远端故障与 malformed response 一律拒绝。
- [x] endpoint 与 HTTP client 借用同一个 host-owned `coro_context`；配置、token、TLS/client 初始化均在 owner 线程启动前完成，错误线程调用在协议转换前失败。

### Task 4: 绑定 standalone endpoint 与部署入口

**Files:**
- Modify: `server/flowie_server.c`
- Modify: `deploy/server/docker-entrypoint.sh`
- Modify: `deploy/server/Dockerfile`

- [x] `flowie_server --config <file> --profile <name> --require-security` 从 YAML 创建 endpoint 与 security binding。
- [x] 保留原命令行开发模式；Docker 必须显式传入配置并要求 security。
- [x] Docker 不再读取 `.flow`，RulesForge/TurboFlow 不进入运行依赖。
- [x] Cluster 默认 OFF，构建与运行依赖扫描中不存在 RulesForge/FlowMQ/TurboRaft。

### Task 5: 验证

- [x] 先运行 config/provider/security 聚焦测试。
- [x] 构建 `flowie_server` 并运行 `--check --require-security` 对真实 EU fixture 验证。
- [x] 将 ORM 的旧 `cflow_source` 名称最小迁移到当前 `cflow_publisher` API，恢复完整 server 链接，并运行安全与 server 相邻回归。
- [x] 更新 CodeGraph 影响面并确认 `.codegraph/` 被 Git 忽略且未纳入提交。
