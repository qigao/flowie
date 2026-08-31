# Flowie Server、Control 与 PostgreSQL

该目录构建一个同时包含 `flowie_server` 与 `flowie-control` 的运行镜像。Compose 创建
`flowie-server` 和 `flowie-postgres` 两个 service；前者由 `flowie-combined-entrypoint` 在同一容器内监督
Server 与 Control 两个独立进程，后者为二者提供同一个 PostgreSQL database。Server 或 Control 任一进程
退出都会终止另一进程并让应用容器重启。两个应用进程都以 UID/GID `10001` 运行，默认只监听 host
loopback。直接运行镜像而不覆盖 entrypoint 时仍保持 Broker-only，但必须挂载 `flowie.yml`；协议库默认
使用 SQLite。

## 运行契约

`flowie_server` 只拥有 MQTT listener、连接、会话、订阅、上下行收发和协议状态。账号、租户、设备及 ACL
以 `flowie-control` Repository 为唯一事实源：MQTT CONNECT 通过 HTTPS `/v4/authenticate` 认证，PUBLISH/
SUBSCRIBE 通过 HTTPS `/v4/acl/check` 逐请求判定。Flowie 不提供业务 HTTP API，也不加载 RulesForge、
TurboFlow、FlowMQ 或 TurboRaft。

入口脚本只装配配置、profile 与协议库：

| 环境变量 | 默认值 | CLI 参数 |
| --- | --- | --- |
| `FLOWIE_CONFIG` | `/etc/flowie/flowie.yml` | `--config`，必须是可读文件 |
| `FLOWIE_PROFILE` | `flowie` | `--profile` |
| `FLOWIE_AUTH_SERVICE_TOKEN_FILE` | 无 | 入口读取 Docker secret 并导出 YAML 引用的 `FLOWIE_AUTH_SERVICE_TOKEN` |
| `FLOWIE_PROTOCOL_STORE_DRIVER` | `sqlite` | `--protocol-store-driver` |
| `FLOWIE_PROTOCOL_STORE_OPTIONS` | `{"filename":"/var/lib/flowie/flowie-protocol.sqlite3"}` | `--protocol-store-options`（JSON object） |
| `FLOWIE_CHECK` | `0` | `--check`（启用时） |

容器内的 `FLOWIE_HEALTH_HOST`/`FLOWIE_HEALTH_PORT` 与 `FLOWIE_HEALTH_SECONDARY_HOST`/
`FLOWIE_HEALTH_SECONDARY_PORT` 只控制健康检查，必须分别指向 MQTT 与 Control listener；Compose 通过
`.env` 中的 `FLOWIE_CONTROL_HEALTH_HOST`/`FLOWIE_CONTROL_HEALTH_PORT` 设置 secondary 探针。向容器传入
显式命令时，入口脚本直接执行该命令。`FLOWIE_CHECK=1` 执行完整的 YAML、service token、TLS client、
security realm、endpoint 与 TurboDB 初始化检查，但不打开网络 listener。布尔值只接受
`0/1/false/true/no/yes/off/on`。endpoint 的 host、port、transport 与容量全部来自 `flowie.yml`，环境变量
不再维护第二份配置；修改 YAML 或 token 后需要重启 broker。

YAML 解析、环境变量读取、TLS/TurboHTTP client 创建均发生在启动阶段。安全 endpoint 与两个出站 HTTP
client 借用同一个 host-owned `coro_context`；MQTT 认证/授权热路径只做有界协议转换、可挂起的
`turbo_http_request()` 和响应解析，不创建 client、线程，也不执行同步网络请求。

连接、session、subscription、inflight 和 retained 分别是并发连接、受管会话总数、单会话订阅数、单会话
待确认 QoS 消息数和 endpoint retained 总数的独立边界。`FLOWIE_SEND_HWM_BYTES` 是每连接待发送字节的
高水位，不是启动时预分配内存；慢连接耗尽该预算时按既有背压策略断开。私有 CoroNet 上下文的 coroutine
pool 容量上界为 `2 × max_connections + 8`，每个 coroutine 都有独立 stack；stream receive buffer
则为每个连接使用的两个 chunk。因此提高连接数、stack 或 receive buffer 前必须计算内存上界并用 RSS
实测校验。socket buffer 只是向内核提出的请求，内核可能按平台策略调整实际值。

`reuse-port` 只改变单 listener 的端口复用选项，不会创建 worker。独立 broker 没有可调 worker 数。建议先用
`flowie_server --check --require-security --config /etc/flowie/flowie.yml --profile flowie
--protocol-store-driver sqlite --protocol-store-options '{"filename":":memory:"}'`
校验 YAML、安全装配和 TurboDB schema，
并保存三条不含 MQTT 身份或内容的
`effective-config` DEBUG 记录。
`max_inflight_per_session` 是服务端 session 的待确认 QoS 消息边界，不是客户端 MQTT 5 Receive Maximum；
容量仍需按单 session 的待发送 QoS 消息峰值评估。

当前独立 broker 以 `FLOWIE_PROTOCOL_STORE_DRIVER` 与 `FLOWIE_PROTOCOL_STORE_OPTIONS` 指定的
TurboDB/Orm repository 作为 session、subscription、inflight、retained、pending Will 和 principal
snapshot 的唯一协议事实源。镜像默认写入 SQLite；Compose 则显式配置 PostgreSQL。启动时打开连接、创建
或校验 Broker V2 schema，任一步失败都会拒绝启动，不回退到进程内存或 SQLite。普通 PUBLISH 业务正文只有
进入显式业务 sink 时才会持久化，不能把 protocol store 当作业务消息库。

Control 使用 `control.yml` 中的 `storage.turbodb.driver/options` 连接数据库。其 V7 Repository 保存 Domain、
用户、credential verifier、Role、Group、ACL、审计以及 management session；原始 session Bearer token
不入库。Compose 示例让 Broker 和 Control 连接同一个 PostgreSQL database，但表名空间独立：Broker 使用
`flowie_server_`，Control 使用 `flowie_control_`。因此无需按模块创建不同 PostgreSQL schema；如需独立
权限、备份或故障域，可将两组连接配置指向不同 database。无论采用哪种部署，升级与备份都必须覆盖两套
逻辑 schema。

## Control 运行契约

Combined 入口内部调用 `flowie-control-entrypoint`。后者默认要求 `/etc/flowie/control.yml` 是普通可读文件，
然后执行：

```text
flowie-control --config /etc/flowie/control.yml
```

`FLOWIE_CONTROL_CONFIG` 可选择其他容器内路径；`FLOWIE_CONTROL_CHECK=1` 添加 `--check` 并只执行完整配置、
TLS 和 secret reference 预检。布尔值只接受 `0/1/false/true/no/yes/off/on`。显式传入命令时 entrypoint
原样执行，不拼接 Control 参数。`FLOWIE_COMBINED_CHECK=1` 会顺序执行 Server 与 Control check-only，且不
打开 MQTT 或 HTTPS listener。

本示例保留 Dashboard，因此 `listener.tls.client_auth` 必须为 `none`，管理员使用首次登录密码进入改密流程。
若部署纯机器控制面，可关闭 Dashboard，改为 `client_auth: required` 并配置 `client_ca_file`；mTLS 只增加
传输端身份校验，Broker 调用 `/v4/authenticate` 和 `/v4/acl/check` 时仍需 Repository 中生成的 service
credential 与对应 Role。

## 构建

默认源码布局包含 Flowie 和五个依赖仓库：

```text
cpp/
  TurboHTTP/
  turbodb/
  turbonet/
    turbo-utils/
    turbo-parser/
    turbonet/
    flowie/
```

从 Flowie 仓库根目录执行：

```sh
export FLOWIE_SOURCE_REVISION="$(git rev-parse HEAD)"
export FLOWIE_SERVER_IMAGE="flowie-server:local"

docker buildx build \
  --file deploy/server/Dockerfile \
  --build-context turbo_utils=../turbo-utils \
  --build-context turbo_parser=../turbo-parser \
  --build-context turbo_net=../turbonet \
  --build-context turbo_db=../../turbodb \
  --build-context turbo_http=../../TurboHTTP \
  --build-arg "SOURCE_REVISION=${FLOWIE_SOURCE_REVISION}" \
  --tag "${FLOWIE_SERVER_IMAGE}" \
  --load \
  .
```

Dockerfile 分别构建并安装五个依赖 SDK，然后从当前 Flowie 源码安装 `flowie_server`、
`flowie-control` 和 `flowie-control-data`。Standalone 镜像固定使用 `FLOWIE_BUILD_CLUSTER=OFF`，构建图和
运行层均不引用 FlowMQ/TurboRaft。构建层与最终运行层分别对三个 executable 执行 `ldd`，任一动态库缺失
都会使镜像构建失败。运行镜像不依赖宿主 SDK、源码或 TurboFlow。发布时应记录镜像 digest，并使用 digest
或不可变 tag 部署。

需要完整 Debug 符号、tlog 文件行号以及 Debug 版依赖时，使用同一个 Dockerfile 的公开 Debug preset：

```sh
docker buildx build \
  --build-arg FLOWIE_BUILD_PRESET=linux-dev-user \
  --build-arg FLOWIE_INSTALL_PRESET=install-linux-dev-user \
  --build-arg FLOWIE_PROFILE=debug \
  --build-arg FLOWIE_ENABLE_ASAN=OFF \
  ...
```

前三个 profile 参数必须成组切换，不能把 Debug/Release SDK 混在同一构建中。原生日志排查默认关闭
ASan；需要 sanitizer 时显式设为 `ON` 并确保运行镜像提供匹配 runtime。运行排查时设置
`FLOWIE_LOG_LEVEL=DEBUG`；Debug 输出包含 source，INFO 生产输出不逐包记录 MQTT 数据。慢订阅者隔离只记录
第 1 次及累计次数为 2 的幂次的摘要；QoS 2 满窗/释放每连接各最多一条。两者均不包含 client ID、用户名、
topic 或 payload。

## Compose 部署

`compose.yml` 定义 `flowie-server` 与 `flowie-postgres`，均使用 host network，适合作为同机
Nginx/HAProxy 后端。MQTT 默认绑定
`127.0.0.1:18883`，Control 默认绑定 `127.0.0.1:8443`。需要对外监听时必须修改对应配置，并在外部代理
或防火墙处配置访问边界。

首次启动前准备部署材料。`config/`、`certs/` 与 `secrets/` 已同时从 Git 和 Docker build context
排除，不会进入镜像层：

```sh
mkdir -p deploy/server/config deploy/server/certs deploy/server/secrets
cp deploy/server/.env.example deploy/server/.env
cp deploy/server/control.yml.example deploy/server/config/control.yml
umask 077
touch deploy/server/secrets/control-key-password
touch deploy/server/secrets/flowie-auth-service-token
```

把现有环境的 `flowie.yml` 保存为 `deploy/server/config/flowie.yml`。其中 endpoint 是 MQTT listener 的唯一
配置源，Auth/ACL channel 指向同容器的 `flowie-control` HTTPS listener，并以
`env://FLOWIE_AUTH_SERVICE_TOKEN` 引用 service credential。将该 credential 的 token 值写入
`deploy/server/secrets/flowie-auth-service-token`；入口只在启动时读取一次，不要把 token 写入 `.env`、YAML、
Compose 文件或镜像层。

将证书链和私钥分别保存为：

```text
deploy/server/certs/flowie-control-server-chain.pem
deploy/server/certs/flowie-control-server-key.pem
```

若私钥加密，通过本机 secret 管理工具或不会留下 shell history 的编辑器，把口令本身写入
`deploy/server/secrets/control-key-password`；不要写 `KEY=value`。不要把 secret 写进 `.env`、Compose YAML、
Control YAML、命令行或镜像。若私钥未加密，从 `config/control.yml` 删除 `key_password_ref`，该 secret 文件
仍需作为显式、权限受控的空文件存在。

`control-key-password` 只解锁 Control HTTPS 服务端私钥，不是 `system/admin` 登录密码；
`flowie-auth-service-token` 是 Broker 调用 Control Auth/ACL API 的 service credential。Vault、云 Secret Manager
或其他第三方平台应在启动前把 secret 原子地落地到该文件，并保持下述 group/mode。entrypoint 只在进程
启动时读取一次，因此更新文件后执行：

```sh
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml \
  up -d --no-deps --force-recreate flowie-server
```

Compose file-backed secret 的 UID/GID/mode 不能重映射，参见
[Docker Compose secrets](https://docs.docker.com/reference/compose-file/services/#secrets)。

配置、证书链与私钥必须可由容器 UID/GID `10001:10001` 读取。以下是 rootful Linux Docker 的一组权限
示例；rootless Docker 或启用 user namespace 时，应改用其映射后的 UID/GID：

```sh
sudo chown -R 10001:10001 deploy/server/config deploy/server/certs
sudo chgrp 10001 deploy/server/secrets/control-key-password \
  deploy/server/secrets/flowie-auth-service-token
chmod 0750 deploy/server/config deploy/server/certs
chmod 0640 deploy/server/config/control.yml deploy/server/config/flowie.yml \
  deploy/server/certs/flowie-control-server-chain.pem
chmod 0600 deploy/server/certs/flowie-control-server-key.pem
chmod 0440 deploy/server/secrets/control-key-password \
  deploy/server/secrets/flowie-auth-service-token
```

先启动 PostgreSQL，再验证 Server 参数、数据库连接/schema 与 Control 配置；不打开应用 listener：

```sh
export FLOWIE_SERVER_IMAGE="flowie-server:local"
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml config
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml \
  up -d --no-build flowie-postgres
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml \
  run --rm --no-deps \
  -e FLOWIE_COMBINED_CHECK=1 flowie-server
```

然后启动应用 service：

```sh
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml \
  up -d --no-build flowie-server
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml ps
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml \
  logs --tail=200 flowie-server
```

空 Control Repository 会建立固定的 `system/admin`，首次密码为公开 bootstrap 值
`Flowie@ChangeMe!`。首次登录 `https://<control-host>/v2/control/dashboard` 后必须立即改密；改密会撤销当前
session，需要重新登录。不得把该公开密码作为生产 secret 或通过网络暴露首次启动中的 Control。
改密后创建第三方 Domain、human 管理账号、密码与最小 Role，并交付 Management/MQTT 接入资料的完整
步骤见 [Flowie 第三方系统接入指南](../../flowie/THIRD_PARTY_INTEGRATION.md)。

容器健康检查同时探测 MQTT 与 Control TCP listener；任一 listener 不可达都会标记容器 unhealthy。运行态
验收还应执行 MQTT 5 CONNECT/CONNACK、QoS 1 publish/subscribe、Control 管理登录，以及
`/v4/authenticate` 与 `/v4/acl/check` 的 allow/deny 路径。升级或迁移前应停止 Control 写入，并备份
`flowie-postgres-data`；恢复时必须保持 Broker V2、Control V7、配置和证书版本的一致性。应用容器升级会
同时中断 MQTT 与 Control。V6 及更早 Control Repository 不会被隐式迁移；切换到本版本前必须执行显式
离线迁移或在确认可丢弃旧 Control 数据后重建数据库。回滚必须使用与备份 schema 匹配的旧镜像。

## 验证与运维

Control 管理 RPC 的最小相关回归为：

```sh
cmake --build --preset linux-dev-user --target \
  test_flowie_control_acl \
  test_flowie_control_store \
  test_flowie_control_management_service \
  test_flowie_control_management_rpc
ctest --preset linux-dev-user --output-on-failure \
  -R '^test_flowie_control_(acl|store|management_service|management_rpc)$'
```

`test_flowie_control_management_rpc` 会在真实 CoroNet coroutine 内提交包含四个 topic entry 的
`control.policy.subject_rule.put`，用于约束 Control 请求路径的 stack budget。该测试必须与普通同步 RPC 测试
同时保留；只在进程主栈上调用 repository 不能覆盖容器内的 coroutine stack 回归。

PostgreSQL live 验收应在不输出密码、Bearer token 或 credential 的前提下完成以下检查：

1. 记录 Flowie 应用容器的 `RestartCount`。
2. 使用已认证的 Management 客户端提交一条业务期望的 `control.policy.subject_rule.put`；要求 HTTP 200、
   JSON-RPC result 含 revision，并能按 request ID 查询到对应审计记录。
3. 再次读取 `RestartCount`，要求与步骤 1 相同；随后冷启动调用方两次，每次都要求 principal reconcile、
   `/v4/authenticate`、`/v4/acl/check` 和 MQTT CONNECT 成功，且计数不变。
4. 失败时先保存 core、容器时间戳日志与 request ID；未满足重启计数不变时不得把健康检查恢复视为通过。

重启计数只需读取，不应为了测试而重置容器：

```sh
docker inspect --format '{{.RestartCount}}' flowie-server
```

```sh
sh deploy/server/tests/test-docker-entrypoint.sh
sh deploy/server/tests/test-flowie-control-entrypoint.sh
sh deploy/server/tests/test-flowie-combined-entrypoint.sh
sh deploy/server/tests/test-healthcheck.sh
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml \
  exec flowie-server flowie_server --help
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml \
  run --rm --no-deps -e FLOWIE_COMBINED_CHECK=1 flowie-server
docker inspect --format '{{.State.Health.Status}}' flowie-server
```

`flowie_server --check --require-security --config <flowie.yml> --profile <name>` 校验 YAML、远程 Auth/ACL
provider、共享 CoroNet context、endpoint，并打开 protocol store 创建或校验 schema 后退出；它不会启动 MQTT
listener，也不会向 flowie-control 发出认证/授权请求。可配合内存 SQLite 做无持久化预检。TLS/WSS listener
投入生产前仍需完成服务端证书路径和握手验收；`flowie.yml` 中的 transport 才是最终有效值。
