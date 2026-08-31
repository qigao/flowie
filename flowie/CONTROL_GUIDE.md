# Flowie Control 部署与配置指南

Control runtime 独立于 MQTT 数据面，组合本地授权事实源、JSON-RPC、HTMX Dashboard，以及可选的
`/v4/authenticate` 与 `/v4/acl/check` 服务。推荐生产入口是 `flowie_server --control-config`，由 Server
Application 统一启动和关闭 Control 与 MQTT；`flowie-control` 独立可执行文件保留用于兼容和诊断。
Auth 可选择本地 Repository verifier，
或只通过 HTTPS 调用一个第三方认证系统；Flowie Broker 不直连身份/ACL 数据库或目录。
控制进程只监听一个显式 HTTPS 地址。空 Control Repository 会自动建立固定首位管理员；
后续用户、Group、Role、credential 与 ACL 只通过管理 RPC 修改。当前仍缺少连接/请求级限流、
HA/migration 和完整安全发布 gate，因此不能据此宣称整个控制面已达到生产就绪。

外部系统接入、认证、并发控制、错误码与 33 个方法的逐项契约见
[Management JSON-RPC API](MANAGEMENT_RPC_API.md)。

## 构建与预检

```powershell
cmake --preset win-release-user
cmake --build --preset win-release-user --target flowie_server flowie-control

$env:FLOWIE_CONTROL_KEY_PASSWORD = "private-key-password"
build\Msvc-Release\bin\flowie-control.exe --check `
  --config flowie\examples\flowie-control.yml
```

真实 listener/Auth/ACL gate 使用临时证书、TurboDB 测试驱动和子进程，覆盖本地 credential 成功/拒绝、
登录会话、逐请求 ACL decision、策略版本不匹配、Repository service credential，以及客户端证书不能代替管理登录；
测试不修改源码树中的部署文件：

```powershell
ctest --preset win-release-user -R test_flowie_control_https_integration --output-on-failure
```

Repository 事务、生产本地 Auth、ACL generation 与 JSON-RPC management service 统一通过 TurboDB
contract 验证。远程打包、测试容器复用、数据清理和证据下载流程见
[`LINUX_REMOTE_TEST_RUNBOOK.md`](LINUX_REMOTE_TEST_RUNBOOK.md)。

也可使用环境变量，优先级固定为 CLI、进程环境、显式 DotEnv：

```powershell
$env:FLOWIE_CONTROL_CONFIG = "C:\flowie\flowie-control.yml"
build\Msvc-Release\bin\flowie-control.exe --check
```

仅在本地开发时使用 `--env-file/-E`。程序不会隐式读取当前目录 `.env`。`--check` 解析完整 schema，
并用 CoroNet/OpenSSL 实际加载 server chain、private key、client CA，以及启用的第三方 HTTPS client
CA/certificate/private key；它不打开 listener、不连接第三方服务或控制数据库，也不执行 schema
migration。证书、私钥、CA 或 secret reference 无法加载时立即失败，不回退到 HTTP、其他 TurboDB
driver 或本地 credential。

## Auth 来源选择

本文中的“HTTP 认证”一律表示使用 HTTPS 的版本化 JSON 契约。企业/第三方集成只允许以下链路：

```text
MQTT Broker -> HTTPS /v4/authenticate -> flowie-control
                                         |
                                         +-> HTTPS third-party assertion service
                                         +-> local authorization Repository
```

Broker v3 请求中的 `remote_address` 由 endpoint transport provenance 提供；默认是 CoroNet 直接 socket
peer，显式启用 trusted PROXY v1/v2 的 TLS/WSS endpoint 则使用已验证 header 中的数值 `IP:port`，并
单独保留 direct transport peer。Pipe 使用 `local`。Flowie 不读取 `X-Forwarded-For` 或其他 HTTP
代理 header。`peer_certificate_sha256` 仅在 MQTT TLS/WSS endpoint 配置
`tls_client_ca_file`、CoroNet 已验证客户端证书后出现；否则是空字符串。该 MQTT 客户证书只描述
MQTT client。Broker 使用 Repository 中具有精确 endpoint Role 的 service principal 调用 Control；
Auth 成功响应中的用户 Domain 才决定后续 ACL policy，Broker service principal 的 Domain 不会成为
业务 Domain，也不会加入 topic。

第三方 HTTPS assertion contract 为严格 version 3，并接收相同的直接 peer address 与可选 MQTT 客户
证书指纹。第三方系统可据此组合自身账户认证，但返回的 external groups/claims 仍必须经过本地
principal 映射；ACL 始终只来自 control Repository。其请求 `domain` 为空表示 Broker 通用认证正在
请求可信 Domain 发现，成功 assertion 必须返回非空 `domain_id`；管理登录等已有目标 Domain 的入口
会发送该 expected Domain，并要求响应精确匹配。

各数据边界固定如下：

| 数据 | 唯一事实源 | Flowie 的行为 |
| --- | --- | --- |
| 第三方 credential、token、目录账户状态 | 第三方 HTTPS 服务 | 发送一次有界请求，只消费 allowlist assertion |
| 本地 user enabled、Domain、Role/Group、ACL | control Repository | 在第三方认证成功后执行本地授权映射 |
| MQTT session、retained、inflight | TurboDB ORM/session store | 不参与身份认证 |
| 第三方 Redis/PostgreSQL/LDAP/AD/OIDC/RADIUS | 第三方服务内部实现 | Broker 与 control 领域核心不加载其 SDK |
| control TurboDB database | control Repository | 只由 `flowie-control` 通过 TurboDB 访问；Broker 不接收连接配置 |

同一请求不会同时查询两个认证来源。第三方拒绝、超时、TLS/协议错误或服务不可用时直接 fail closed，
不会验证本地密码、切换 TurboDB ORM/database backend 或匿名放行。若未来需要第三方 profile/目录查询，
也必须增加独立 HTTPS 契约，不能新增进程内 directory/database adapter。

## 配置 schema

根节点只接受 `version`、`listener`、`storage`、`management`、`dashboard` 和 `auth`；
未知字段、错误类型、重复证书指纹与越界值都会使启动失败。完整示例见
[flowie-control.yml](examples/flowie-control.yml)。

| 路径 | 约束 |
| --- | --- |
| `version` | 当前只接受 `1` |
| `listener.host` | 默认 `127.0.0.1`；生产应显式配置 loopback 或管理网地址 |
| `listener.port` | `1..65535`，默认 `8443` |
| `listener.coroutine_stack_size` | Control listener 每个协程的栈字节数，默认且最小 `262144`，最大 `2097152`；修改后需重启 |
| `listener.tls.cert_file` | 必填，PEM server certificate chain |
| `listener.tls.key_file` | 必填，PEM private key |
| `listener.tls.key_password_ref` | 可选，只接受 `env://UPPER_CASE_NAME`，不接受 literal |
| `listener.tls.client_auth` | `none`（默认）或 `required`；Dashboard 启用时只能为 `none` |
| `listener.tls.client_ca_file` | 仅 `client_auth: required` 时必填；`none` 时禁止配置 |
| `listener.limits.*` | Iris header、URL、JSON、body 与 header count 的有界配额 |
| `storage.turbodb.driver` | 必填的 TurboDB driver 名称；运行时按该值选择已构建的 TurboDB provider，缺少对应 component 时启动失败且不回退 |
| `storage.turbodb.options` | 最多 16 个字符串键值；`conninfo`、`password`、`sslpassword`、`uri`、`url` 必须使用 `env://UPPER_CASE_NAME`，其他值原样交给 TurboDB |
| `management.rpc_path` | 静态绝对 path，默认 `/v2/control/rpc` |
| `management.session.capacity` | 同一 Repository 中的有界登录会话数，默认 `1024`，最大 `65536`；超过容量时按持久化 LRU 撤销 |
| `management.session.max_sessions_per_principal` | 每个 `(domain, principal)` 最多保留的会话数，默认 `5`，最大 `65536`；新登录撤销该主体最早签发的会话 |
| `management.session.ttl_seconds` | 会话上限，默认 `3600`，最大 `86400`；不会超过认证 principal expiry |
| `management.login_executor.*` | 本地管理登录的 `workers/queue_capacity/deadline_ms`，默认 `4/128/10000`；任一外部 Auth provider 启用时禁止显式配置 |
| `dashboard.enabled` | 是否注册固定 HTMX Dashboard 路由 |
| `auth.enabled` | 是否注册 `/v4/authenticate` 与 `/v4/acl/check`；默认关闭 |
| `auth.local_executor.workers` | 本地 Auth 同步 verifier worker 数，`1..64`，默认 `4` |
| `auth.local_executor.queue_capacity` | 本地 Auth 等待队列容量，`1..4096`，默认 `128`；满载返回 429 |
| `auth.local_executor.deadline_ms` | 本地 Auth HTTP 等待上限，`1..60000`，默认 `10000`；到期返回 503 |
| `auth.external_https` | 可选；缺失时使用本地 Auth，出现时由第三方 HTTPS Auth 替换本地 verifier |
| `auth.external_https.url` | 必须为带明确 path 的 HTTPS URL；userinfo、query、fragment 均拒绝 |
| `auth.external_https.service_token_ref` | 访问第三方服务的 bearer token，只接受独立 `env://...` reference |
| `auth.external_https.trusted_issuer` | 第三方断言必须精确匹配的 issuer |
| `auth.external_https.subject_type` | 第三方断言必须精确匹配的 subject type |
| `auth.external_https.timeout_ms` | `1..30000`，默认 `3000` |
| `auth.external_https.max_response_size` | `1024..65536`，默认 `16384` |
| `auth.external_https.max_in_flight` | `1..1024`，默认 `64`；满载时在读取 token 和发起网络请求前返回 busy |
| `auth.external_https.tls.*` | 可选私有 CA；client certificate/key 必须成对出现，密码只接受 `env://...` |
| `auth.jwt_jwks` | 可选；与 `local_executor`、`external_https` 互斥，本地验证 JWT，JWKS 只从固定 HTTPS URL 获取 |
| `auth.jwt_jwks.url` | 必须为带明确 path 的 HTTPS URL；userinfo、query、fragment 均拒绝，redirect/retry 禁用 |
| `auth.jwt_jwks.trusted_issuer` / `audience` / `subject_type` | 必填并精确匹配；`sub` 与请求 identity 精确匹配，签名后的 `domain_id` 选择业务 Domain；已有目标 Domain 的管理入口还会执行精确约束 |
| `auth.jwt_jwks.algorithm` | 必填的单一非对称算法：ES/PS/RS、ES256K 或 EdDSA；拒绝 `none` 与 HMAC |
| `auth.jwt_jwks.max_response_size` / `max_keys` / `max_token_size` | 默认 `65536/16/4096`，硬上限 `1048576/64/16384` |
| `auth.jwt_jwks.refresh_interval_seconds` / `clock_skew_seconds` | 默认 `300/30`，硬上限 `86400/300`；过期 snapshot 不延寿 |
| `auth.jwt_jwks.executor.*` | 密码学验证和 JWKS 解析的有界 worker/queue/deadline，默认 `4/128/10000` |
| `auth.jwt_jwks.tls.ca_file` | 可选私有 CA；启动时加载校验，运行时始终验证 HTTPS peer |

当 `auth.enabled: true` 时还必须设置 `listener_id` 和 `method`。Broker caller 必须是 Repository 中
enabled 的 service principal，持有 generated credential，并按 endpoint 分配 `flowie_auth_client` 或
`flowie_acl_client`；Control YAML 不保存 service binding 或 token。auth cache 容量最大 4096，TTL 最大 60 秒，
同一组容量/TTL 限制同时约束 positive credential cache 与 principal snapshot cache。principal cache
命中仍会复核 user/credential revision、全局 store revision 与 policy version，任一事实源不可用时
fail closed。
认证服务还会在 Argon2id/KDF 前执行双层 token bucket：默认每个已验证 service caller 为
`100 requests/s`、burst `200`，每个 `(caller, domain, principal)` 为 `5 requests/s`、burst `10`。
连续失败消耗 identity bucket；成功凭据只清除该 identity 的失败 bucket，不能重置 caller 总量。
桶有界且只保留 keyed digest，不保存 identity、证书指纹或 secret 明文。
本地 Auth 的 Argon2id 与同步 TurboDB 调用只进入专用 executor。Iris request/response/socket
不跨线程；deadline 只结束 HTTP 等待，不会强行取消正在执行的同步 KDF/SQL。迟到结果被丢弃，任务自行
擦除 secret；endpoint shutdown 停止接单并 drain。显式 `local_executor` 与两个外部 provider 互斥。
外部网络 I/O 始终留在 CoroNet coroutine 路径，JWT/JWK 解析和签名验证进入其专用有界 executor。
YAML 不保存 ACL rule body、用户 credential、service token 或私钥内容。

唯一数据库边界示例：

```yaml
storage:
  turbodb:
    driver: sqlite
    options:
      filename: data/flowie-control.sqlite3
      open_mode: read_write_create
      busy_timeout_ms: "1000"
```

旧 `storage.control_store`、`storage.sqlite` 与 `storage.postgresql` 会作为未知字段拒绝；没有兼容 parser、
provider fallback 或双写路径。driver 与 option 的具体契约由所安装的 TurboDB package 定义。

Control 当前 Repository schema 为 V7。V7 新增持久化 management session、签发序列与过期/LRU 索引；
session 原始 Bearer token 不入库，只保存 32-byte digest、CSRF、主体、过期时间和访问顺序。进程重启后，
未过期且未撤销的 session 仍可解析；每次请求仍从 Repository 重新读取用户与角色，因此禁用主体或撤销角色
会立即生效。V6 及更早 schema 会 fail fast，不在启动时隐式迁移；升级前必须停止写入、备份，并通过显式
离线迁移或重建 Repository 完成 V7 切换。

## 首位管理员 bootstrap

bootstrap 只解决空 Repository 尚无系统管理员可登录的问题，不是第二条管理通道。身份和公开初始凭据
固定为 `system/admin` / `Flowie@ChangeMe!`，不接受 YAML、环境变量或命令行覆盖。旧版
`bootstrap:` 配置会作为未知字段被拒绝，升级配置时必须删除；Docker secret env file 也不再需要
`FLOWIE_BOOTSTRAP_PASSWORD`。

runtime 仍通过同一 Repository command/audit 边界创建 `system` Domain、启用用户、本地 credential、
`system_admin` 与 `password_change_required` Role 及 assignment，不直接执行数据库 SQL。账号从创建起持有
两个持久角色，但 `password_change_required` 存在时，有效权限严格收缩为改密；公开默认密码不能用于其他
管理 RPC。

启动顺序固定为：创建/验证 Repository schema，执行 bootstrap，创建管理 session store，最后打开 listener。
七个领域命令具有固定 request ID 和提交顺序，因此进程在中途退出后可安全重放。初始密码只参与首次
credential 创建；bootstrap 完成后重启只验证系统管理员结构，不用初始密码覆盖或验证当前密码。
无关的非空 Repository 会返回 `TURBO_EBUSY` 并阻止 listener 启动，不会覆盖已有身份或恢复默认密码。

首次登录 `system/admin` 后只能进入 `/v2/control/password`；改密成功会轮换 credential、移除
`password_change_required` 并撤销当前 session。重新登录后，系统管理员通过 `control.domain.create` 创建
`root-a/root-b/...`，再显式为目标 root 创建首位用户，通过 `control.password.set` 的 `create` mode
设置人类密码，并创建、分配 `security_admin` 角色。管理员重置已有密码时必须显式使用 `replace`
mode，不会从 create 失败自动降级为替换。后续只通过登录 session 保护的 JSON-RPC 创建/禁用用户，生成/轮换/
撤销 credential，维护 Group/Role，验证并发布 ACL；不得再次添加 bootstrap，也不得直接修改数据库。

Dashboard 对该流程提供同一组命令的可视化入口。`system/admin` 重新登录后，在 Overview 的
“Third-party platform setup” 中先创建并切换到第三方专属 Domain，再在 Users 中创建 `human` 用户。
对该用户执行 “Set password” 时，首次设置必须显式选择 `create`；只有确认覆盖已有密码时才选择
`replace`。两种模式都要求操作者输入并确认密码，Control 不生成、不回显 human 密码。`service` 用户
不会显示该密码操作，而是使用独立的 “Issue token”；生成的 token 只在成功响应中显示一次。

升级前必须备份非空 Control Repository。由相同 `system/admin` bootstrap 建立且已改密的 Repository 可以
直接重放校验；旧版若配置了其他 bootstrap username，其现有数据不会自动迁移，新版本会 fail closed，
需要先通过旧版本管理 RPC 迁移到固定系统身份或恢复旧版本与数据库备份。

仓库提供一次性 operator 工具执行上述五个 RPC。密码只从当前进程环境读取，不接受命令行密码参数；
每个命令使用由 system principal、Domain 和首位管理员派生的固定 request ID。重复执行会走
Repository replay；每步只依赖前一步成功，因此中断后可继续，最终还会以新业务管理员重新登录
验证 credential 与 Domain 绑定：

```powershell
$env:FLOWIE_SYSTEM_ADMIN_PASSWORD = "<current-system-admin-password>"
$env:FLOWIE_ROOT_ADMIN_PASSWORD = "<new-root-admin-password>"

pwsh ./deploy/provision-root.ps1 `
  -ControlUrl https://mqtt.dev.my-photo.xyz `
  -RootGroup root-a `
  -AdminPrincipal admin-a

Remove-Item Env:FLOWIE_SYSTEM_ADMIN_PASSWORD
Remove-Item Env:FLOWIE_ROOT_ADMIN_PASSWORD
```

`control.password.set(create)` 不会把密码写入 replay 结果，因此相同 request ID 的恢复执行可能返回
`-32009`。工具只对这一步延迟判定，继续补齐 Role 与 assignment，然后必须使用
`FLOWIE_ROOT_ADMIN_PASSWORD` 完成目标 Domain 登录和 `control.system.status` 校验后才报告成功。校验失败
会立即终止；工具不会自动切换到 `replace`，以免覆盖不属于本次恢复流程的现有密码。

该工具只用于从 `system/admin` 创建业务管理域。Broker、PicImpact 或其他业务服务不得持有
`system_admin` 凭据，也不得自行创建 Domain；它们只使用已 provision 的最小权限业务管理员或
service credential。

第三方平台管理 Control 时，应在自己的 Domain 中使用独立 principal，并由平台后端通过
`/v2/control/login` 获取短期 management session，再以该 session 调用 `/v2/control/rpc`。浏览器端
不得持有管理密码或 session，也不能从其他 Origin 直接登录。按职责组合保留角色：只读使用
`viewer`，Group 管理使用 `viewer + user_admin`，ACL 管理使用 `viewer + policy_admin`；只有确实负责
整个 Domain 的平台才使用 `security_admin`。

此处的 management session 与 generated credential 相互独立。Repository service principal 的
generated credential 只保护 Broker 到 `/v4/authenticate`、`/v4/acl/check` 的调用；MQTT principal 的
generated credential 可作为 MQTT password。两者都不能作为 `/v2/control/rpc` 的 Bearer token，
也不得相互复用。
session 在过期、显式撤销或容量淘汰后由第三方后端重新登录获取；Flowie 重启不会撤销尚未过期的
持久 session。禁用 principal 或撤销管理角色会使现有 session 的后续请求立即失去权限。

机器凭据 RPC 返回 `{"token":"flw_mqtt_v1_..."}`。MQTT CONNECT 的三个字段必须按规范分开配置：
`FLOWIE_MQTT_USERNAME` 是 Control `principal_id` 对应的 User Name，用于认证和授权；
`FLOWIE_DEVICE_TOKEN` 是 MQTT Password，必须原样发送；`FLOWIE_MQTT_CLIENT_ID` 是独立的 MQTT
Client Identifier，用于 Session、takeover、订阅状态和集群路由。Client ID 可以是随机/哈希风格的
非空字符串，不是凭据，也不从 Username 派生；需要持久 Session 时，重连必须复用同一个 Client ID。
不得 Base64 解码 Token，也不得继续使用旧的 Base64 credential 环境变量。
`/v4/authenticate` 请求中的 `secret_base64` 仅是 Broker 在 HTTP JSON 中
传输 MQTT Password 字节的内部编码，不是设备配置格式。升级现有设备必须 rotate credential 并原子替换
配置；旧二进制 credential 没有兼容或 fallback 路径。

这是 OASIS MQTT 5.0 的标准语义：Client Identifier 见
[`3.1.3.1`](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc385349242)，
User Name 见 [`3.1.3.5`](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc385349245)。
标准没有要求二者相等。Flowie 启用认证时要求 User Name 非空；Client ID 遵循 MQTT 版本规则，使用
HAProxy 的 cluster 接入部署还会在代理层要求 Client ID 非空。

未配置外部 provider 时，`/v4/authenticate` 使用 Repository 中的本地 credential verifier；配置
`external_https` 或 `jwt_jwks` 时，它严格替换本地 verifier，失败不会回退本地密码。不在 `flowie-control` 进程内
加载 OIDC、LDAP/AD、RADIUS 或第三方数据库 SDK；它们全部由第三方 HTTPS 服务内部处理。

本地 Auth 的最小配置如下：

```yaml
auth:
  enabled: true
  listener_id: flowie-control-auth
  method: bearer
  local_executor:
    workers: 4
    queue_capacity: 128
    deadline_ms: 10000
```

第三方 Auth 在同一块增加：

```yaml
auth:
  enabled: true
  listener_id: flowie-control-auth
  method: bearer
  external_https:
    url: https://third-party-auth.internal/v3/assert
    service_token_ref: env://FLOWIE_THIRD_PARTY_AUTH_TOKEN
    trusted_issuer: https://identity.internal
    subject_type: device
    timeout_ms: 3000
    max_response_size: 16384
    max_in_flight: 64
    tls:
      ca_file: certs/third-party-auth-ca.pem
      client_cert_file: certs/flowie-auth-client-chain.pem
      client_key_file: certs/flowie-auth-client-key.pem
      client_key_password_ref: env://FLOWIE_THIRD_PARTY_AUTH_KEY_PASSWORD
```

Broker 请求同时发送 generated service token、`X-Flowie-Service-Id` 与
`X-Flowie-Service-Domain`；Control 从 Repository 校验 principal、credential 和 endpoint Role。
`auth.external_https.service_token_ref` 则保护 `flowie-control` 到第三方断言服务的请求，两类 token
具有不同的信任方向、权限和轮换周期，不能复用。该 provider 与管理登录、Broker Auth endpoint
共享同一 authenticator 实例和本地 subject mapper。

JWT/JWKS Auth 示例：

```yaml
auth:
  enabled: true
  listener_id: flowie-control-auth
  method: bearer
  jwt_jwks:
    url: https://identity.internal/.well-known/jwks.json
    trusted_issuer: https://identity.internal
    audience: flowie
    subject_type: device
    algorithm: EdDSA
    max_keys: 16
    max_token_size: 4096
    refresh_interval_seconds: 300
    executor:
      workers: 4
      queue_capacity: 128
      deadline_ms: 10000
    tls:
      ca_file: certs/identity-ca.pem
```

`POST /v4/acl/check` 只接受具有 `flowie_acl_client` Role 的受信 service bearer。请求 body 的
principal Domain 来自 Broker 先前获得的 Auth principal；MQTT 客户端不能通过 path、query、header
或自定义 JSON 指定 Domain。响应是 version 4 的单次 allow/deny decision，不返回 ACL bundle。

第三方成功断言的 `issuer` 和 `subject_type` 必须精确匹配配置，稳定 `subject` 被解释为当前 Domain
中的本地 `principal_id`。Repository 随后重新检查该 principal 存在且 enabled，并只从本地事实源加载
Role/Group；第三方 groups 只是有界映射输入，不会自动获得本地权限。第三方拒绝、超时、TLS/协议错误、
主体不存在或本地用户禁用都 fail closed，且不回退到本地密码。并发达到 `max_in_flight` 时返回 busy，
不会先读取 service token，也不会建立额外连接。

Broker service token 每次请求都从其 secret provider 获取，可独立轮换。Control 验证的 service
credential、principal enabled 状态和 endpoint Role 以 Repository 为事实源；rotate/revoke 或 Role
移除后请求 fail closed。

HTTPS adapter 内部提供不含 identity、credential、token、URL 或响应内容的统计快照，字段包括
`started_requests`、`in_flight`、`succeeded`、`denied`、`local_overload`、`remote_overload`、
`remote_server_failures`、`transport_failures`、`protocol_failures` 和 `local_failures`。应分别对本地
舱壁、远端 429、远端 5xx、传输失败和协议失败计算窗口 rate；不要把认证拒绝率直接当作服务可用性故障。
快照为逐字段无锁采样，并发读取时不保证跨字段事务一致。不得改为逐请求记录 token、identity 或请求/
响应正文。

具有 `security_admin` 角色且持有有效管理登录会话的调用方，可从现有
`management.rpc_path` 查询全局聚合快照：

```json
{"jsonrpc":"2.0","method":"control.auth.external_https.stats","params":{},"id":1}
```

启用第三方 HTTPS 认证时，结果包含 `enabled: true` 和上述十个计数；未启用时只返回
`{"enabled":false}`。该方法不接受其他参数。计数跨 Domain 聚合，因此 root-scoped `viewer`、
`user_admin` 和 `policy_admin` 均无权读取；未认证返回 `-32001`，无权限返回 `-32003`，未知参数返回
JSON-RPC `-32602`。响应继承管理 RPC 的 `Cache-Control: no-store`、禁用 batch/notification 和请求配额。

该 RPC 已提供安全的诊断采集入口，但不会计算窗口 rate、SLO 或告警。生产发布前仍需配置外部 collector、
阈值基线和 runbook；不应另外开放匿名 metrics listener。

`principal_ttl_seconds` 同时定义已连接 Broker session 的撤销传播上界。principal 到期时，即使连接完全
空闲且没有 `recv_timeout_ms`/keepalive，Broker 也会主动 fail closed：MQTT 5 先发送
`DISCONNECT 0x87` 再关闭，MQTT 3.x 直接关闭。MQTT 5 客户端必须在到期前发起 Enhanced AUTH
re-authentication；成功提交的新 principal 会原子替换旧 expiry deadline。禁用用户、轮换或撤销
credential 仍不会建立控制面到 Broker 的即时 push 通道，最坏传播时间由当前 principal TTL 决定。

## Domain 数据导出与导入

安装产物 `flowie-control-data` 用于迁移一个非 `system` Domain 的声明式管理数据。构建时
`tbe_compiler` 从 `control/schema/flowie_control_data.schema` 生成 SQLite 与 PostgreSQL DDL；工具使用
版本化 SQLite manifest（`.db`）作为可移植交换格式。manifest 包含 User、Group 层级、直接 membership、
Role、直接 Role assignment、ACL draft 与发布状态，不包含密码/verifier、service token、management
session 或历史 audit。它不是物理数据库备份，也不能替代升级前的完整备份。

导出和导入必须在停止 Control/Flowie Server 写入后执行，并使用与 Control 相同的 YAML 和 secret 环境。
导出拒绝覆盖已有文件；它先写同目录临时文件，读取前后 revision 一致后才原子发布：

```bash
flowie-control-data export \
  --config /etc/flowie/flowie-control.yml \
  --domain booth \
  --output /var/backups/flowie/booth-domain.db
```

导入前先执行 dry-run。dry-run 会检查格式版本、唯一 Domain、引用完整性和 canonical ACL，读取目标
Repository revision，但不执行领域写命令：

```bash
flowie-control-data import \
  --config /etc/flowie/flowie-control.yml \
  --input /var/backups/flowie/booth-domain.db \
  --dry-run

flowie-control-data import \
  --config /etc/flowie/flowie-control.yml \
  --input /var/backups/flowie/booth-domain.db
```

实际导入按 Domain、User、Group 深度、Role、直接关系、ACL、publish 的依赖顺序调用共享 Management
Service，因此 revision 与 audit 仍由 Control Repository 作为唯一事实源推进，不直接写 Control 表。
目标应是刚重建且除固定 `system` bootstrap 外为空的 Repository；唯一允许的其他非空状态是同一
manifest 上一次中断后留下的可重放前缀。
工具不把 manifest 当作增量补丁，也不会覆盖或协调与本次导入无关的既有 Domain 数据。
每条命令使用稳定 request ID；进程在中途失败时，修正外部原因后可用同一 manifest 重跑，已经成功的前缀
按幂等 replay 处理。导入不会生成 credential；完成后须由管理员按正常 Management RPC/Dashboard 流程为
service User 签发新 token。若需回退，停止写入并恢复操作前另行保存的物理数据库备份。

`--env-file <path>` 可在解析数据库 `env://` secret 前加载显式 DotEnv 文件。禁止导出或导入 `system`
Domain，避免把固定系统管理员身份当作业务迁移数据。

## ACL 文法与发布

当前 ACL 按 `role`、`group` 或 `user` 主体维护。用户的有效 Role/Group 规则与自身 User 规则共同
求值，任一匹配 deny 优先，否则任一匹配 allow 生效，未匹配时默认拒绝。顶层 `allow`/`deny` 控制
MQTT CONNECT；文档中的 `read`、`write`、`readwrite` 分别控制 SUBSCRIBE、PUBLISH 或两者。topic
使用 `<domain>/<bounded MQTT filter>`，支持完整 segment 的 `%u` username、`%c` client ID、MQTT
`+`/终端 `#` wildcard，以及终端 alternatives；`groups`、`devices` 不再是固定结构关键字。

完整 grammar、canonical 格式、主体约束、容量限制，以及 Control UI/Management RPC 的
draft、validate、publish 流程见 [ACL_GRAMMAR.md](ACL_GRAMMAR.md)。旧的 pipe-delimited internal rule
不是 Control ACL 输入格式。

## 登录会话与管理权限

管理请求的 actor 不能来自 header、JSON-RPC params 或 Dashboard form。管理员在
`/v2/control/login` 提交 Domain、principal 和密码；认证成功后服务端签发随机不透明 token。
浏览器只得到 `Secure; HttpOnly; SameSite=Strict` cookie，RPC 也可提交同一 token 作为 bearer。每次请求
都从 Repository 重新解析当前 enabled 状态和保留角色，因此禁用账户或撤销管理角色会立即使已有会话
失权。登录表单还要求精确同源 `Origin`/`Host`，管理写操作继续要求会话内独立 CSRF token。本地密码
验证在专用有界 executor 中执行；队列满返回 429，deadline 到期返回 503，超时请求随后产生的 session
会由 worker 撤销。第三方 HTTPS Auth 使用 CoroNet coroutine I/O，不进入该 executor。

```text
verified login credential in the presented Domain
  -> opaque bounded server-side session
  -> current enabled principal in the selected control Repository
  -> current effective roles
  -> password-change-only 或 viewer/user_admin/policy_admin/security_admin/system_admin permission bits
```

`password_change_required` 存在时会屏蔽其他管理权限，直到 `control.password.change` 或 Dashboard
改密页成功完成 credential rotate 与角色移除。`system_admin` 只能位于 `system` 域，可创建业务 Root
Group 并显式管理目标域；业务 `security_admin` 永远不能跨 Domain。任一步失败都返回未授权。
保留角色是精确字符串；其他业务角色不会获得管理权限。principal 必须在
所选 Repository 中已存在、启用且至少拥有一个保留角色。空 Repository 必须使用上文
一次性 bootstrap；不能通过直接编辑 TurboDB 底层数据绕过领域事务、revision 与审计不变量。

Management RPC 当前注册 33 个方法，其中 `control.domain.list` 只允许登录在 `system` Root 且拥有
`system_admin` 的 caller 调用。它以 Repository 的 Domain 记录作为唯一事实源，并提供
`after`/`limit` keyset 分页；Domain 不是 Group，也不会出现在 effective Groups 中。

本节说明部署与权限模型；外部调用者需要的请求参数、返回结构、错误码、分页和幂等语义以
[Management JSON-RPC API](MANAGEMENT_RPC_API.md) 为准。

支持目标作用域的 read/list/write RPC 可携带可选 `domain_id`；省略时使用登录 session 的 Root。
该字段不会改变登录身份：服务端会重新解析当前 principal 和保留角色，再调用共享 management service
验证目标 Root。只有 `system/system_admin` 可以选择其他已存在 Root，且查询与修改采用相同规则；
普通 `security_admin` 即使伪造其他 `domain_id` 也会得到 JSON-RPC forbidden `-32003`。目标 Root
不存在时返回 not found，而不是隐式创建或回退到登录 Root。actor 始终来自登录 session，不能由 header、
RPC params 或 Dashboard form 覆盖。

## 启动与关闭

推荐的组合启动：

```powershell
build\Msvc-Release\bin\flowie_server.exe `
  --control-config C:\flowie\flowie-control.yml `
  --require-security `
  C:\flowie\control.yml C:\flowie\control.flow
```

Server Application 先创建并同步绑定 Control HTTPS listener，再启动 MQTT worker；MQTT 启动失败会关闭
Control listener。正常关闭顺序是先停止 MQTT，再在 Control 所属 CoroNet context 线程取消连接、关闭
listener、停止 context 并 join。以下独立入口仅用于诊断：

```powershell
build\Msvc-Release\bin\flowie-control.exe `
  --config C:\flowie\flowie-control.yml
```

正常启动只调用显式 host 的 `iris_app_listen_tls_on()`；默认
`TURBO_TLS_CLIENT_AUTH_NONE`。只有 Dashboard 关闭且显式配置 `listener.tls.client_auth: required` 时，
才启用高安全服务端 mTLS 部署能力。独立入口收到 `SIGINT`/`SIGTERM` 后停止 event loop；runtime 随后先解除
Auth、Dashboard 和 RPC 绑定，再销毁 app、caller-owned RPC context、service 与 TurboDB Repository。
Repository 关闭失败会返回进程错误而不是静默退出。

Dashboard 入口为 `/v2/control/dashboard`，只显示状态概览；管理数据集使用独立页面：
`/v2/control/dashboard/users`、`/groups`、`/roles`、`/acls` 和 `/audit`（均以 Dashboard 入口为
路径前缀）。页面共享登录 session、CSRF 保护和领域 command/query service，但每次只读取并渲染当前
数据集。`system/system_admin` 会看到 Domain 选择器：它提示 `control.domain.list` 返回的前
100 个 Root，也接受手工输入其他已存在 Root；当前 `domain_id` 会保留在页面导航、查询、keyset
分页和所有 HTMX CRUD 中。普通 Root 管理员不显示该选择器，手工构造跨 Root query/form 仍返回 403。
在 `system` scope 的 Overview 中，`system_admin` 可通过 **Add domain** 创建新 Domain；切换到目标
Domain 后，Users 页面可创建 `principal_type: service` 的用户。拥有当前 Domain `security_admin`
权限的 caller 可在该 service 用户行签发或撤销 token：首次签发调用 generate，已有 credential 时同一
操作调用 rotate 并立即使旧 token 失效。明文 token 只出现在该次成功 POST 的 HTML 结果中，不进入
session、URL、审计或持久化明文；页面刷新、关闭提示或响应丢失后无法恢复，只能用新的 request ID 再次
签发。关闭提示前会先清空 DOM 中的 token；服务端在发送响应后擦除包含 token 的 HTML buffer。
用户的 Group membership 在 Users 页面的 Access 操作中维护：add/remove 都从当前 Root 的 Group 树选择，
选项按 parent/depth 深度优先排列；Root 节点不能成为 membership。创建 Group 时也从同一树选择
enabled 且未达到最大深度的 parent。Groups 页面只管理树节点本身的创建与禁用，不再提供第二套
Members 写入口。
RPC 使用配置的 `management.rpc_path`，认证服务固定为 `POST /v4/authenticate`。反向代理必须
保留 `Host`，并使用公共或内部受信 TLS；不能把代理添加的身份 header 当作登录、Broker service identity
或 Domain 来源。
