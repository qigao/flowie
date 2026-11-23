# Flowie 服务端使用指南

`flowie_server` 支持 MQTT 3.1、3.1.1 与 5，监听方式为 TCP、TLS、WS 或 WSS。
它负责 MQTT 连接、会话、订阅、消息投递和协议持久化。Control 作为独立进程提供管理、
Auth 和 ACL 服务。容器内的进程组合与配置见 [Compose 部署指南](../deploy/server/README.md)。

## 构建

按[根 README](../README.md#build-and-test)恢复依赖后，在同一 PowerShell 会话中执行：

```powershell
cmake --preset native-release-user
cmake --build --preset native-release-user
```

Windows LLVM-MinGW 构建产物为 `build/native-release-user/bin/flowie_server.exe`。
完整参数由 `flowie_server --help` 列出。原生进程可由系统服务管理器监管。
当前 CMake 目标提供 `flowie_server`，仓库中的旧 supervisor 源码保留为历史实现。

## 用 CLI 启动本地监听

```powershell
& ./build/native-release-user/bin/flowie_server.exe --host 127.0.0.1 --port 1883 --transport tcp
```

CLI 默认绑定 `0.0.0.0:1883`，WS/WSS 默认路径为 `/mqtt`。
这个本地例子未配置 Auth 或 ACL，显式绑定 loopback。切换到配置式运行时，由
`--config` 和 `--profile` 选择 YAML 中的 endpoint、容量参数和安全 provider。
`--profile` 默认为 `flowie`，YAML 模式下 endpoint 取值来自所选 profile，CLI 的 listener 和容量参数
不覆盖它。协议存储仍由 CLI 或进程环境单独配置。

## 校验和启动安全配置

先准备生产环境自己的 YAML、证书、secret reference 和协议存储配置。
测试仓库中的 [HTTPS 配置 fixture](../flowie/server/tests/flowie_server_https_secure.yml)
展示 profile、endpoint、HTTPS Auth/ACL channel 与 security realm 的结构，其中的服务地址和凭据引用是测试值。

```powershell
& ./build/native-release-user/bin/flowie_server.exe --check --require-security --config C:/flowie/flowie.yml --profile flowie
```

`--require-security` 要求 YAML 同时配置远程 HTTPS Auth 和 ACL。`--check` 执行配置解析、
provider 和 endpoint 装配，并实际打开所选 TurboDB repository、创建或校验 schema。
它不绑定 MQTT listener，也不验证远端 Auth/ACL 可达或 TLS 握手成功。成功时输出
`flowie_server: options are valid`。去掉 `--check` 后启动同一配置。

`flowie_server` 读取 YAML 或 CLI。将 endpoint 接入 TurboFlow Graph 是应用宿主的组合方式，
其状态归属与 settlement 约束见 [配置式 Broker 概念](CONFIGURED_BROKER_CONCEPTS.md)。

## TCP、TLS、UDP、KCP、WS 与 WSS

TCP/TLS 由 CNet 承载，WS/WSS 由 Chttp App 承载。底层 endpoint runtime 也包含 UDP/KCP，
standalone CLI 的 MQTT 监听范围为 TCP/TLS/WS/WSS。

TLS/WSS listener 使用 Salts 进程环境中的证书和私钥：

```powershell
$env:SALTS_TLS_CERT_FILE = "C:/certs/server-chain.pem"
$env:SALTS_TLS_KEY_FILE = "C:/certs/server-key.pem"
```

WS/WSS 客户端发送 `Sec-WebSocket-Protocol: mqtt`，请求路径必须与 endpoint 配置精确匹配。
反向代理应保留该路径与子协议。MQTT 报文使用二进制帧，单帧与分片重组后的消息都受 endpoint
的报文大小上限约束。

配置 `tls_client_ca_file` 后，TLS/WSS endpoint 要求并验证客户端证书，且必须绑定
`security_realm`。验证成功后，客户端证书的 SHA-256 指纹进入 Auth 请求。
这是 MQTT client 到 Broker 的身份信息。Broker 到 Auth 服务的 HTTPS 客户端证书单独配置。

TCP/TLS 可用 `--network-workers`、`--network-policy` 和 `--network-cpus` 选择固定 owner 数量、
连接分配和 CPU 亲和性。WS/WSS 可配置 owner 数量和分配策略。socket 缓冲、keepalive、
命令容量和关闭约束见[网络 owner 说明](ARCHITECTURE.md#tcptls-network-owners)。

## HTTPS 认证和 ACL

安全 profile 的 `auth_provider` 选择 HTTPS Auth channel，endpoint 的 `security_realm`
通过 `policy_source` 选择 HTTPS ACL channel。Broker 使用受限 scope 的 service credential
调用 Control 或兼容服务，成功返回的 MQTT principal 决定业务 Domain。
service token 通过 key-provider reference 获取。TLS、响应契约或 principal 校验失败时拒绝认证。

内置 HTTPS provider 支持一次性 MQTT 5 Authentication Data 验证。多轮 challenge 需要由应用宿主
通过 enhanced provider ABI 实现。API、mTLS、凭据轮换和 ACL 快照边界见
[HTTPS Auth/ACL 决策](ADR_HTTPS_AUTH_SERVICE.md)和 [Control 指南](CONTROL_GUIDE.md)。
公网边缘代理示例见 [Nginx 与 HAProxy 部署](../deploy/nginx/README.md)。

## Session、retained 与持久化

standalone server 使用 TurboDB `Orm::C` repository 保存 session、subscription、inflight、Will、
retained 和 principal snapshot。使用 `--protocol-store-driver` 与 `--protocol-store-options` 指定驱动
和 JSON 字符串 options。对应环境变量为 `FLOWIE_PROTOCOL_STORE_DRIVER` 与
`FLOWIE_PROTOCOL_STORE_OPTIONS`，CLI 优先。

原生默认是 SQLite 和当前目录的 `flowie-protocol.sqlite3`。执行仅使用内存 SQLite 的预检：

```powershell
& ./build/native-release-user/bin/flowie_server.exe --check --protocol-store-driver sqlite --protocol-store-options '{"filename":":memory:"}'
```

默认驱动模块由构建时选择的 SDK 提供。部署路径变化时，可在 options 中提供指向管理员控制的
`.so` 或 `.dll` 的绝对路径 `flowie.driver_module`。最多 16 个 options。模块加载、连接、schema
或记录恢复失败会使启动失败。

Broker 与 Control 可连接同一个 PostgreSQL database，各自使用 `flowie_server_` 与
`flowie_control_` namespace。业务 PUBLISH 正文需要由应用显式写入自己的 sink。
实验集群采用独立的 TurboRaft log/snapshot，见 [MQTT 集群归属](ADR_MQTT_CLUSTER_OWNERSHIP.md)。

standalone broker 启动时会建立 `flowie_server_replaced_wills`。使用 `create_schema = 0` 的自建
协议仓库需预先建表。回滚前应让待派发的接管 Will 清空。 [仓库升级说明](ADR_TYPED_ORM_PERSISTENCE_V2.md#pending-will-after-session-replacement)

显式订阅 `$SYS/broker/version` 或 `$SYS/#` 会在 ACL 允许时收到 broker 版本，使用 retained
QoS 0，并遵守 MQTT 5 Retain Handling。版本主题由 broker 发布，客户端写入会被拒绝。其他
`$SYS/` 主题遵守现有 ACL。根 `#` 和 `+/#` 不会收到系统主题。

## 部署验收

对实际配置运行 `--check --require-security`，再验证 listener、证书和主机名、Auth/ACL 允许与
拒绝、QoS 收发及重启后的 session 恢复。可运行的 CI 和产品验收范围见
[SDK 资格与产品发布要求](RELEASE_GATE.md)。
