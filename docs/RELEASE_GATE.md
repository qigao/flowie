# Flowie SDK 资格与产品发布要求

## 当前自动化

[原生 SDK 资格验证](../.github/workflows/native-sdk-qualification.yml)调用
[共享管线](../.github/workflows/native-sdk-pipeline.yml)，构建、安装和打包 Linux GCC、Windows MSVC、
Android arm64 SDK。Linux 和 Windows job 通过 CTest 运行其配置注册的用例。
Android job 检查安装产物的 16 KiB ELF 对齐。版本 tag 通过
[SDK 发布 workflow](../.github/workflows/native-sdk-release.yml)发布归档、manifest 和校验和。

本地复验使用 [根 README](../README.md#build-and-test) 中的 SDK 恢复、完整构建和 CTest 命令。
已有场景和待补覆盖见 [MQTT 测试矩阵](MQTT_TEST_MATRIX.md)，固定 broker 的准备见
[Mosquitto 互操作环境](../interop/mosquitto-2.0.22/README.md)。
`test_flowie_mqtt_client_live` 只验证公共 broker 的连通性。固定 broker 互操作使用独立测试配置。

## 产品发布验收

下列门槛适用于 MQTT 协议、客户端、Broker endpoint、应用宿主和持久化的组合验收。
仓库当前仅声明 `FLOWIE_MQTT_RELEASE_GATE`，尚未注册综合 manifest 自检或证据校验脚本。
执行实际注册的分项测试时，记录源码 revision、平台、SDK/backend 版本、TLS 模式、结果和未执行的用例。
长稳验证还需记录同 revision 的 corpus、soak、sanitizer、seed 与资源增长证据。
原生 SDK 资格验证只覆盖其各 job 注册和执行的测试。

## 固定门槛

- MQTT flowie/protocol/client：MQTT 3.1/3.1.1/5 编解码、QoS 0/1/2、订阅、取消订阅、PING、断线、MQTT 5
  Enhanced AUTH/re-auth、CONNACK 发送能力约束与有界 command queue 必须通过；固定 broker interop
  必须覆盖 TCP/TLS/WS/WSS。公网 live suite 仅用于人工确认外网访问能力。
- Server flowie/transport：当前发布基线仅为 TCP/TLS/WS/WSS。已注册的
  `test_flowie_transport_baseline` 必须在 MQTT 3.1、3.1.1 与 5 上分别完成真实
  CONNECT、SUBSCRIBE、QoS 0/1/2 PUBLISH ACK、UNSUBSCRIBE、PING 与正常 DISCONNECT；TLS/WSS
  必须实际校验证书和主机名，不能用配置解析代替握手。UDP 与 Unix Pipe 列为 TODO，不属于当前 gate。
  framing、WS admission、TLS/mTLS 失败矩阵、异常 shutdown 和资源回收的完整要求按
  [`MQTT_TEST_MATRIX.md`](MQTT_TEST_MATRIX.md) 的 NET/SEC ID 逐项进入已注册测试；未迁移的旧 turbo-flow
  测试不得计为证据。
- Session/authorization：CONNECT、QoS1/QoS2、重复包、session takeover、retained message、shared
  subscription、Will/Will delay、session expiry、Topic Alias、Subscription Identifier、Assigned Client
  Identifier、Receive Maximum send window、Keep Alive、Enhanced AUTH/re-auth、default-deny ACL 与
  未授权 publish/subscribe 均须通过。
- Backpressure/resource：per-connection 与 aggregate send HWM、慢订阅者隔离、最大连接数、session、
  subscription、inflight 和 retained 容量都必须有确定的拒绝与终态，不允许 silent drop。
- Persistence：`flowie_server --check` 必须创建并扫描部署所选 TurboDB record store。
  PostgreSQL 部署需连接隔离的真实 PostgreSQL 验证此路径；
  endpoint 重建、binary retained key、pending Will、incompatible record 以及 provider commit 故障注入
  必须通过。CONNECT 的 durable commit 失败不得发送 CONNACK 或留下半提交 session；请求持久化时不得
  回退到进程内状态。
- Authentication provider：`profiles.<name>.auth_provider` 必须精确选择唯一注册的 `https` backend；
  `http://`、userinfo、query、fragment、数据库字段、literal token、未知字段和无效 secret reference
  必须在 listener 启动前 fail fast。运行时 TLS、超时、状态码、Content-Type、协议版本和 principal
  校验失败必须拒绝认证，不得 fallback。`flowie-control` 必须明确选择本地 Auth 或唯一
  `external_https`；选择外部模式时不得再尝试本地 verifier、TurboDB ORM 或其他数据库认证路径。
   使用外部模式的发布证据必须显示 `flowie.auth.external_https.stats` 的 `enabled: true`；本地 Auth
   模式应为 `enabled: false`，并通过本地 credential contract、executor queue-full 429、deadline
   503 和 shutdown drain gate。显式 `local_executor` 与 `external_https` 必须在配置期判定为冲突。
- Product host：standalone 验证 YAML、provider 与 repository 装配、`--check` 和进程关闭。
  Compose 部署验证 Broker/Control 子进程启动、退出和清理。采用 TurboFlow 的宿主另验 Graph 与 RuleSet。

## 明确边界

本 gate 证明库级 security binding/本地 ACL 行为及 bundled `flowie_server` 的配置驱动 provider 装配。
Flowie 只访问 HTTPS Auth/ACL 服务，不接受 Redis/SQLite/PostgreSQL 或其他身份/ACL 数据库连接配置；
数据库网络 ACL 必须只允许 `flowie-control` 或第三方认证服务访问。认证服务 token 只能通过
key-provider reference 注入。TLS 始终校验服务主机名；
私有 CA 与 mTLS 客户端证书可按 provider 注入，私钥密码只能通过 key-provider reference 获取。生产网络按配置
限制 Broker 出站到 HTTPS Auth/ACL 和所选协议数据库。认证数据库只向认证服务开放。内置 HTTPS provider 当前把 MQTT 5 Authentication Data
作为一次性 HTTPS credential 完成认证；需要多轮 challenge 的部署必须注入实现 enhanced provider ABI
的认证模块。真实 mTLS 验收需要观察双向证书验证成功，以及缺失客户端证书时握手被拒绝。
多轮认证能力以部署所选 provider 的契约为准。

没有隔离数据库的常规 CTest 配置可以省略 PostgreSQL live suite，发布证据中应记录其执行状态。
