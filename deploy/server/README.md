# Flowie server and Control 单容器

该目录构建一个同时包含 `flowie_server` 与 `flowie-control` 的运行镜像。Compose 只创建一个
`flowie-server` service，由 `flowie-combined-entrypoint` 在同一容器内监督两个独立进程；任一进程退出都会
终止另一进程并让容器重启。当前 server 仍直接接收 listener 参数，不读取 Flowie config、graph 或
embedded Control 配置。两个进程都以 UID/GID `10001` 运行，默认只监听 host loopback。直接运行镜像而
不覆盖 entrypoint 时仍保持 Broker-only，避免破坏既有 `docker run` 使用方式。

## 运行契约

入口脚本把以下环境变量映射到 `flowie_server` 的同名参数：

| 环境变量 | 默认值 | CLI 参数 |
| --- | --- | --- |
| `FLOWIE_HOST` | `127.0.0.1` | `--host` |
| `FLOWIE_PORT` | `18883` | `--port` |
| `FLOWIE_TRANSPORT` | `tcp` | `--transport` |
| `FLOWIE_PATH` | `/mqtt` | `--path` |
| `FLOWIE_MAX_PACKET_SIZE` | `1048576` | `--max-packet-size` |
| `FLOWIE_MAX_CONNECTIONS` | `1024` | `--max-connections` |
| `FLOWIE_MAX_SESSIONS` | 跟随 `FLOWIE_MAX_CONNECTIONS` | `--max-sessions` |
| `FLOWIE_MAX_SUBSCRIPTIONS_PER_SESSION` | `1024` | `--max-subscriptions-per-session` |
| `FLOWIE_MAX_INFLIGHT_PER_SESSION` | `64` | `--max-inflight` |
| `FLOWIE_MAX_RETAINED_MESSAGES` | 跟随 `FLOWIE_MAX_SESSIONS` | `--max-retained-messages` |
| `FLOWIE_SEND_HWM_BYTES` | `1048576` | `--send-hwm-bytes` |
| `FLOWIE_COROUTINE_STACK_SIZE` | `0`（组件默认值） | `--coroutine-stack-size` |
| `FLOWIE_STREAM_RECV_BUFFER_BYTES` | `0`（组件默认值） | `--stream-recv-buffer-bytes` |
| `FLOWIE_SOCKET_RECV_BUFFER_BYTES` | `0`（操作系统默认值） | `--socket-recv-buffer-bytes` |
| `FLOWIE_SOCKET_SEND_BUFFER_BYTES` | `0`（操作系统默认值） | `--socket-send-buffer-bytes` |
| `FLOWIE_TIMEOUT_MS` | `0`（禁用默认超时） | `--timeout-ms` |
| `FLOWIE_RECV_TIMEOUT_MS` | `0`（跟随默认超时） | `--recv-timeout-ms` |
| `FLOWIE_TCP_KEEPALIVE` | `0` | `--tcp-keepalive` |
| `FLOWIE_TCP_KEEPALIVE_IDLE_MS` | `0`（操作系统默认值） | `--tcp-keepalive-idle-ms` |
| `FLOWIE_TCP_KEEPALIVE_INTERVAL_MS` | `0`（操作系统默认值） | `--tcp-keepalive-interval-ms` |
| `FLOWIE_TCP_KEEPALIVE_COUNT` | `0`（操作系统默认值） | `--tcp-keepalive-count` |
| `FLOWIE_REUSE_PORT` | `0` | `--reuse-port` |
| `FLOWIE_LOG_LEVEL` | `INFO` | `--log-level` |
| `FLOWIE_CHECK` | `0` | `--check`（启用时） |

容器内的 `FLOWIE_HEALTH_HOST`/`FLOWIE_HEALTH_PORT` 与 `FLOWIE_HEALTH_SECONDARY_HOST`/
`FLOWIE_HEALTH_SECONDARY_PORT` 只控制健康检查，必须分别指向 MQTT 与 Control listener；Compose 通过
`.env` 中的 `FLOWIE_CONTROL_HEALTH_HOST`/`FLOWIE_CONTROL_HEALTH_PORT` 设置 secondary 探针。向容器传入
显式命令时，入口脚本直接执行该命令，例如 `flowie_server --check --port 18883`。`FLOWIE_CHECK=1` 让入口
按完整环境变量生成参数后执行 check-only。
布尔环境变量只接受 `0/1/false/true/no/yes/off/on`，其他值会在启动前失败。keepalive 的 idle、interval
或 count 非零时必须同时启用 `FLOWIE_TCP_KEEPALIVE`。全部参数均在进程启动时确定，修改环境变量后需要重启
broker；当前不支持运行时热更新。

连接、session、subscription、inflight 和 retained 分别是并发连接、受管会话总数、单会话订阅数、单会话
待确认 QoS 消息数和 endpoint retained 总数的独立边界。`FLOWIE_SEND_HWM_BYTES` 是每连接待发送字节的
高水位，不是启动时预分配内存；慢连接耗尽该预算时按既有背压策略断开。私有 CoroNet 上下文的 coroutine
pool 容量上界为 `2 × max_connections + 32`，每个 coroutine 都有独立 stack；stream receive buffer
则为每个连接使用的两个 chunk。因此提高连接数、stack 或 receive buffer 前必须计算内存上界并用 RSS
实测校验。socket buffer 只是向内核提出的请求，内核可能按平台策略调整实际值。

`reuse-port` 只改变单 listener 的端口复用选项，不会创建 worker。独立 broker 没有可调 worker 数；多
worker/supervisor 拓扑属于完整的 Flowie YAML runtime，不在这个容器入口的职责范围内。建议先用
`flowie_server --check --log-level DEBUG ...` 校验参数，并保存三条不含 MQTT 身份或内容的
`effective-config` DEBUG 记录。
`64` 是库级 session inflight 默认值，不是客户端 MQTT 5 Receive Maximum。高并发诊断可显式设置
`FLOWIE_MAX_INFLIGHT_PER_SESSION=1024`；容量仍需按单 session 的待发送 QoS 消息峰值评估。

当前独立 broker 的 session、subscription、inflight、retained 和 pending Will 都是进程内状态；容器重启
后不会恢复。`flowie-data` 保留为以后扩展的数据边界，但当前 broker 不把 MQTT 状态写入其中。
`flowie-control-data` 不同：它嵌套挂载到 `/var/lib/flowie/control`，保存 Control SQLite Repository，包含
Domain、用户、credential verifier、Role、Group、ACL 与审计，必须纳入备份。两个卷保持独立事实与恢复
边界，即使它们由同一个容器挂载。

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

默认源码布局是八个同级仓库：

```text
cpp/
  TurboHTTP/
  flowmq/
  turbodb/
  turboraft/
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
  --build-context flow_mq=../../flowmq \
  --build-context turbo_raft=../../turboraft \
  --build-arg "SOURCE_REVISION=${FLOWIE_SOURCE_REVISION}" \
  --tag "${FLOWIE_SERVER_IMAGE}" \
  --load \
  .
```

Dockerfile 分别构建并安装七个依赖 SDK，然后从当前 Flowie 源码安装 `flowie_server` 和
`flowie-control`。构建层与最终运行层分别对两个 executable 执行 `ldd`，任一动态库缺失都会使镜像构建
失败。运行镜像不依赖宿主 SDK、源码或 TurboFlow。发布时应记录镜像 digest，并使用 digest 或不可变
tag 部署。

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

`compose.yml` 只定义一个 `flowie-server` service，并使用 host network，适合作为同机 Nginx/HAProxy
后端。MQTT 默认绑定
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
```

将证书链和私钥分别保存为：

```text
deploy/server/certs/flowie-control-server-chain.pem
deploy/server/certs/flowie-control-server-key.pem
```

若私钥加密，通过本机 secret 管理工具或不会留下 shell history 的编辑器，把口令本身写入
`deploy/server/secrets/control-key-password`；不要写 `KEY=value`。不要把 secret 写进 `.env`、Compose YAML、
Control YAML、命令行或镜像。若私钥未加密，从 `config/control.yml` 删除 `key_password_ref`，该 secret 文件
仍需作为显式、权限受控的空文件存在。

这里的 secret 只解锁 Control HTTPS 服务端私钥，不是 `system/admin` 登录密码。Vault、云 Secret Manager
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
sudo chgrp 10001 deploy/server/secrets/control-key-password
chmod 0750 deploy/server/config deploy/server/certs
chmod 0640 deploy/server/config/control.yml \
  deploy/server/certs/flowie-control-server-chain.pem
chmod 0600 deploy/server/certs/flowie-control-server-key.pem
chmod 0440 deploy/server/secrets/control-key-password
```

先验证 Server 参数与 Control 配置，不打开 listener 或 SQLite：

```sh
export FLOWIE_SERVER_IMAGE="flowie-server:local"
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml config
docker compose --env-file deploy/server/.env -f deploy/server/compose.yml \
  run --rm --no-deps \
  -e FLOWIE_COMBINED_CHECK=1 flowie-server
```

然后启动唯一 service：

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
`flowie-control-data`；恢复时必须保持 Repository、配置和证书版本的一致性。单容器升级会同时中断 MQTT
与 Control。若需回滚，使用旧镜像和旧版 Compose 重新创建两个 service，并复用原 `flowie-data` 与
`flowie-control-data`；本改动不迁移数据库格式。

## 验证与运维

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

`flowie_server --check` 校验 listener 参数并退出，不启动服务。TLS/WSS 虽是 CLI 可选 transport，但投入生产
前仍需完成证书配置路径和握手验收；当前已验证的容器默认契约是 TCP。
