# Flowie server container

该目录构建和运行独立的 Flowie MQTT broker。Flowie 不依赖 TurboFlow；当前 server 直接接收 listener
参数，不读取 Flowie config、graph 或 embedded Control 配置。镜像以 UID/GID `10001` 运行，默认只监听
`127.0.0.1:18883`。

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

`FLOWIE_HEALTH_HOST` 和 `FLOWIE_HEALTH_PORT` 只控制容器健康检查；它们必须指向实际 listener。向容器传入
显式命令时，入口脚本直接执行该命令，例如 `flowie_server --check --port 18883`。
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
后不会恢复。命名卷保留为以后扩展的数据边界，但当前 broker 不把 MQTT 状态写入其中。

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

Dockerfile 分别构建并安装七个依赖 SDK，然后从当前 Flowie 源码安装 `flowie_server`。运行镜像不依赖宿主
SDK、源码或 TurboFlow。发布时应记录镜像 digest，并使用 digest 或不可变 tag 部署。

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

`compose.yml` 使用 host network，适合作为同机 Nginx/HAProxy 后端。默认 listener 仅绑定 loopback；需要
对外监听时必须显式设置 `FLOWIE_HOST`，并在外部代理或防火墙处配置访问边界。

```sh
export FLOWIE_SERVER_IMAGE="flowie-server:local"
docker compose -f deploy/server/compose.yml config
docker compose -f deploy/server/compose.yml up -d --no-build flowie-server
docker compose -f deploy/server/compose.yml ps
docker compose -f deploy/server/compose.yml logs --tail=200 flowie-server
```

健康检查确认 PID 1 存活并能连接 `FLOWIE_HEALTH_HOST:FLOWIE_HEALTH_PORT`。运行态验收还应执行 MQTT 5
CONNECT/CONNACK 和 QoS 1 publish/subscribe，不能只依赖 TCP 探针。

## 验证与运维

```sh
sh deploy/server/tests/test-docker-entrypoint.sh
docker compose -f deploy/server/compose.yml exec flowie-server flowie_server --help
docker inspect --format '{{.State.Health.Status}}' flowie-server
```

`flowie_server --check` 校验 listener 参数并退出，不启动服务。TLS/WSS 虽是 CLI 可选 transport，但投入生产
前仍需完成证书配置路径和握手验收；当前已验证的容器默认契约是 TCP。
