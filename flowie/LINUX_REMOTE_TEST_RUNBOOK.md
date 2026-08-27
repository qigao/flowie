# Flowie Linux 远程测试 Runbook

本文用于从 Windows 工作站打包当前工作树，将 TurboUtils、TurboParser、TurboNet、TurboDB、TurboHTTP、
FlowMQ、TurboRaft 和 Flowie 上传到 `root@eu:/root/dev`，在隔离目录中构建八个仓库，从同一份源码构建 Flowie server Docker
镜像，启动 run-scoped 固定 Mosquitto，并运行 Flowie MQTT release/nightly cases。远端已有 PostgreSQL、
Redis 或 Nginx 属于外部基础设施，本 runbook 只读取其状态，不创建、重启或清理它们。

## 1. 执行边界

- Windows 工作树是本次源码事实源；打包包含未提交文件，但排除 `.git/`、构建树、vcpkg 安装树、
  CodeGraph 索引和 `.env`。
- 远端每次使用 `/root/dev/runs/<run-id>`；源码、宿主机测试 SDK 和证据均保存在该 run 目录，
  不覆盖宿主机 `/opt` 下已有包。Dockerfile 内部的 `/opt/<package>/release` 只存在于隔离构建 stage。
- 本次创建的 Docker 端口只绑定 `127.0.0.1`；不得修改已有 PostgreSQL、Redis 或 Nginx 的端口和凭据。
- 所有命令 fail fast。不要用发布必需用例的跳过、Disabled 或仅编译结果替代测试成功；
  可选 public broker smoke 默认 Disabled。
- release 与 nightly 分开：release 使用 GCC；libFuzzer/nightly 使用 Clang 独立构建树。
- PostgreSQL、Redis 属于可选外部 release evidence；固定 broker 属于本次 run 的证据。可选 public broker
  smoke 只验证公网访问能力。
- 当前 Flowie server transport 发布基线仅为 TCP/TLS/WS/WSS，四者均须提供端到端证据。UDP 与 Unix
  Pipe 列为 TODO；CoroNet/runtime adapter 的低层通过不代表 server 已支持，也不计入本 run 的成功条件。
- Flowie server 镜像必须由本次解包的八个源码目录构建；不得复用宿主机 SDK、预编译二进制或浮动镜像。
  发布证据失败时保留本次 run 目录和日志。

## 2. Windows：打包并上传当前源码

在 PowerShell 中执行。八个源码目录应分别为：

- `C:\projects\cpp\turbonet\turbo-utils`
- `C:\projects\cpp\turbonet\turbo-parser`
- `C:\projects\cpp\turbonet\turbonet`
- `C:\projects\cpp\turbonet\turbodb`
- `C:\projects\cpp\TurboHTTP`
- `C:\projects\cpp\turbonet\flowmq`
- `C:\projects\cpp\turbonet\turboraft`
- `C:\projects\cpp\turbonet\flowie`

```powershell
$ErrorActionPreference = 'Stop'
$sourceRoot = 'C:\projects\cpp'
$artifactRoot = Join-Path $sourceRoot 'artifacts'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$bundleBase = "flowie-stack-$stamp"
$bundleName = "$bundleBase.zip"
$bundle = Join-Path $artifactRoot $bundleName
$manifestName = "$bundleBase.revisions.txt"
$manifest = Join-Path $artifactRoot $manifestName
$checksum = "$bundle.sha256"

New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

$repositories = [ordered]@{
    'turbo-utils' = Join-Path $sourceRoot 'turbonet\turbo-utils'
    'turbo-parser' = Join-Path $sourceRoot 'turbonet\turbo-parser'
    'turbonet'     = Join-Path $sourceRoot 'turbonet\turbonet'
    'turbodb'      = Join-Path $sourceRoot 'turbonet\turbodb'
    'TurboHTTP'    = Join-Path $sourceRoot 'TurboHTTP'
    'flowmq'       = Join-Path $sourceRoot 'turbonet\flowmq'
    'turboraft'    = Join-Path $sourceRoot 'turbonet\turboraft'
    'flowie'       = Join-Path $sourceRoot 'turbonet\flowie'
}

$revisionLines = foreach ($entry in $repositories.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath (Join-Path $entry.Value 'CMakePresets.json'))) {
        throw "missing repository: $($entry.Value)"
    }
    $revision = git -C $entry.Value rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw "git revision failed: $($entry.Value)" }
    $dirtyEntries = @(git -C $entry.Value status --porcelain=v1 --untracked-files=all)
    if ($LASTEXITCODE -ne 0) { throw "git status failed: $($entry.Value)" }
    "$($entry.Key) commit=$revision dirty=$([int]($dirtyEntries.Count -gt 0)) entries=$($dirtyEntries.Count)"
}
$revisionLines | Set-Content -LiteralPath $manifest -Encoding utf8

tar.exe -a -cf $bundle `
    --exclude='.git' --exclude='*/.git' `
    --exclude='.codegraph' --exclude='*/.codegraph' `
    --exclude='.worktrees' --exclude='*/.worktrees' `
    --exclude='.lake' --exclude='*/.lake' `
    --exclude='turbonet/turbo-utils/build' `
    --exclude='turbonet/turbo-utils/vcpkg_installed' `
    --exclude='turbonet/turbo-parser/build' `
    --exclude='turbonet/turbo-parser/vcpkg_installed' `
    --exclude='turbonet/turbonet/build' `
    --exclude='turbonet/turbonet/vcpkg_installed' `
    --exclude='turbonet/turbodb/build' `
    --exclude='turbonet/turbodb/vcpkg_installed' `
    --exclude='TurboHTTP/.tmp' `
    --exclude='TurboHTTP/build' `
    --exclude='TurboHTTP/vcpkg_installed' `
    --exclude='turbonet/flowmq/build' `
    --exclude='turbonet/flowmq/vcpkg_installed' `
    --exclude='turbonet/turboraft/build' `
    --exclude='turbonet/turboraft/vcpkg_installed' `
    --exclude='turbonet/flowie/build' `
    --exclude='turbonet/flowie/vcpkg_installed' `
    --exclude='.env' --exclude='.env.*' --exclude='*.log' `
    -C $sourceRoot `
    'turbonet/turbo-utils' 'turbonet/turbo-parser' 'turbonet/turbonet' `
    'turbonet/turbodb' 'TurboHTTP' 'turbonet/flowmq' 'turbonet/turboraft' 'turbonet/flowie' `
    -C $artifactRoot $manifestName
if ($LASTEXITCODE -ne 0) { throw 'source archive failed' }

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bundle).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($checksum, "$hash  $bundleName`n", [Text.Encoding]::ASCII)

ssh root@eu 'install -d -m 0700 /root/dev/incoming /root/dev/runs'
if ($LASTEXITCODE -ne 0) { throw 'remote directory preparation failed' }
scp $bundle $checksum root@eu:/root/dev/incoming/
if ($LASTEXITCODE -ne 0) { throw 'source upload failed' }
ssh root@eu "printf '%s\n' '$bundleName' > /root/dev/incoming/latest-bundle"
if ($LASTEXITCODE -ne 0) { throw 'remote bundle marker failed' }

Write-Host "uploaded=$bundleName sha256=$hash"
```

`tar.exe` 直接读取当前工作树，因此不会漏掉尚未 commit 的测试实现。归档前仍应人工确认没有真实密钥、
生产 `.env` 或数据库 dump 位于源码目录。

## 3. Linux：进入持久会话并准备 run 目录

```bash
ssh root@eu
cd /root/dev
tmux new-session -s flowie-eu
```

后续命令在同一个 `tmux` shell 中执行。需要断开 SSH 时按 `Ctrl-b d`；重新进入使用
`tmux attach-session -t flowie-eu`。

Debian/Ubuntu 主机先准备基线工具；`/opt/vcpkg` 必须由运维预装并固定到团队批准的 revision：

```bash
apt-get update
apt-get install -y build-essential clang cmake ninja-build git curl zip unzip pkg-config \
  openssl ca-certificates tmux docker.io docker-buildx-plugin docker-compose-plugin \
  mosquitto-clients libpq-dev
systemctl start docker
test -x /opt/vcpkg/vcpkg
```

```bash
set -Eeuo pipefail
umask 077

BUNDLE_NAME="$(cat /root/dev/incoming/latest-bundle)"
case "$BUNDLE_NAME" in
  flowie-stack-*.zip) ;;
  *) echo "invalid bundle name: $BUNDLE_NAME" >&2; exit 1 ;;
esac

cd /root/dev/incoming
sha256sum -c "$BUNDLE_NAME.sha256"

RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_ROOT="/root/dev/runs/$RUN_ID"
SRC_ROOT="$RUN_ROOT/src"
SDK_ROOT="$RUN_ROOT/pkgs"
ARTIFACT_ROOT="$RUN_ROOT/artifacts"
BUNDLE="/root/dev/incoming/$BUNDLE_NAME"
SOURCE_REVISION="$(sha256sum "$BUNDLE" | awk '{print $1}')"

install -d -m 0700 "$SRC_ROOT" "$SDK_ROOT" "$ARTIFACT_ROOT"
unzip -q "$BUNDLE" -d "$SRC_ROOT"

TURBO_UTILS_SRC="$SRC_ROOT/turbonet/turbo-utils"
TURBO_PARSER_SRC="$SRC_ROOT/turbonet/turbo-parser"
TURBO_NET_SRC="$SRC_ROOT/turbonet/turbonet"
TURBO_DB_SRC="$SRC_ROOT/turbonet/turbodb"
TURBO_HTTP_SRC="$SRC_ROOT/TurboHTTP"
FLOW_MQ_SRC="$SRC_ROOT/turbonet/flowmq"
TURBO_RAFT_SRC="$SRC_ROOT/turbonet/turboraft"
FLOWIE_SRC="$SRC_ROOT/turbonet/flowie"

for source_dir in "$TURBO_UTILS_SRC" "$TURBO_PARSER_SRC" "$TURBO_NET_SRC" "$TURBO_DB_SRC" \
                  "$TURBO_HTTP_SRC" "$FLOW_MQ_SRC" "$TURBO_RAFT_SRC" "$FLOWIE_SRC"; do
  test -f "$source_dir/CMakePresets.json"
  test -f "$source_dir/CMakeUserPresets.json"
done

{
  date -u --iso-8601=seconds
  uname -a
  cmake --version
  ninja --version
  gcc --version | head -n 1
  clang --version | head -n 1
  docker --version
  docker buildx version
  docker compose version
  /opt/vcpkg/vcpkg version
  printf 'source_revision=%s\n' "$SOURCE_REVISION"
} | tee "$ARTIFACT_ROOT/environment.txt"
```

必需工具为 CMake、Ninja、GCC/G++、Clang、Docker Buildx、Docker Compose、OpenSSL、`unzip`、`tmux`、
`mosquitto_pub/sub` 和 `/opt/vcpkg`。缺失时先安装并重新执行本节；不要在工具缺失状态继续。

## 4. 从本次源码构建并验证 Flowie server Docker 镜像

本节必须在任何源码目录执行 CMake configure/build 之前完成。Windows 归档已排除各仓库的 `build/` 和
`vcpkg_installed/`；保持这个顺序可确保 BuildKit named contexts 只包含本次归档源码，不会混入随后在
Linux 主机生成的 SDK 或编译产物。

本节复用 `deploy/server/Dockerfile`。named contexts 分别指向本次 run 解包出的七个依赖源码目录；
Dockerfile 会在独立 stage 中构建并安装七个依赖 SDK，再从 Flowie 源码生成仅包含 `flowie_server`、
运行库和部署资源的非 root runtime 镜像。镜像构建不替代后续 release/nightly 测试 gate。

```bash
cd "$FLOWIE_SRC"

FLOWIE_SERVER_IMAGE="flowie-server:run-${RUN_ID,,}"
case "$FLOWIE_SERVER_IMAGE" in
  flowie-server:run-*) ;;
  *) echo "invalid Flowie server image tag: $FLOWIE_SERVER_IMAGE" >&2; exit 1 ;;
esac

docker buildx inspect --bootstrap \
  2>&1 | tee "$ARTIFACT_ROOT/flowie-server-buildx-builder.txt"

docker buildx build \
  --file "$FLOWIE_SRC/deploy/server/Dockerfile" \
  --build-context "turbo_utils=$TURBO_UTILS_SRC" \
  --build-context "turbo_parser=$TURBO_PARSER_SRC" \
  --build-context "turbo_net=$TURBO_NET_SRC" \
  --build-context "turbo_db=$TURBO_DB_SRC" \
  --build-context "turbo_http=$TURBO_HTTP_SRC" \
  --build-context "flow_mq=$FLOW_MQ_SRC" \
  --build-context "turbo_raft=$TURBO_RAFT_SRC" \
  --build-arg "SOURCE_REVISION=$SOURCE_REVISION" \
  --tag "$FLOWIE_SERVER_IMAGE" \
  --progress=plain \
  --load \
  "$FLOWIE_SRC" \
  2>&1 | tee "$ARTIFACT_ROOT/flowie-server-image-build.log"

IMAGE_REVISION="$(docker image inspect \
  --format '{{index .Config.Labels "org.opencontainers.image.revision"}}' \
  "$FLOWIE_SERVER_IMAGE")"
test "$IMAGE_REVISION" = "$SOURCE_REVISION"
test "$(docker image inspect --format '{{.Config.User}}' "$FLOWIE_SERVER_IMAGE")" = "10001:10001"

{
  sh "$FLOWIE_SRC/deploy/server/tests/test-docker-entrypoint.sh"
  docker run --rm --entrypoint /bin/sh "$FLOWIE_SERVER_IMAGE" -c '
    set -eu
    test "$(id -u)" = 10001
    test "$(id -g)" = 10001
    ldd /usr/local/bin/flowie_server > /tmp/flowie-server.ldd
    cat /tmp/flowie-server.ldd
    ! grep -F "not found" /tmp/flowie-server.ldd
  '
  set +e
  docker run --rm "$FLOWIE_SERVER_IMAGE" flowie_server --help \
    > "$ARTIFACT_ROOT/flowie-server-help.txt" 2>&1
  FLOWIE_HELP_RC=$?
  set -e
  test "$FLOWIE_HELP_RC" -eq 1
  grep -F 'usage: flowie_server [OPTIONS...]' "$ARTIFACT_ROOT/flowie-server-help.txt"
  printf 'flowie_server_help_exit_code=%s\n' "$FLOWIE_HELP_RC"
} 2>&1 | tee "$ARTIFACT_ROOT/flowie-server-image-smoke.log"

FLOWIE_RUNTIME_PORT=18885
FLOWIE_RUNTIME_CONTAINER="flowie-runtime-${RUN_ID,,}"
case "$FLOWIE_RUNTIME_CONTAINER" in
  flowie-runtime-*) ;;
  *) echo "invalid Flowie runtime container name: $FLOWIE_RUNTIME_CONTAINER" >&2; exit 1 ;;
esac
if ss -ltnH | awk '{print $4}' | grep -Eq "[:.]${FLOWIE_RUNTIME_PORT}$"; then
  echo "Flowie runtime test port already in use: $FLOWIE_RUNTIME_PORT" >&2
  exit 1
fi

cleanup_flowie_runtime() {
  docker rm -f "$FLOWIE_RUNTIME_CONTAINER" >/dev/null 2>&1 || true
}
trap cleanup_flowie_runtime EXIT

docker run --detach \
  --name "$FLOWIE_RUNTIME_CONTAINER" \
  --network host \
  --read-only \
  --tmpfs /tmp:size=64m,mode=1777 \
  --tmpfs /run:size=16m,mode=0755 \
  --env FLOWIE_PORT="$FLOWIE_RUNTIME_PORT" \
  --env FLOWIE_HEALTH_PORT="$FLOWIE_RUNTIME_PORT" \
  "$FLOWIE_SERVER_IMAGE"

for attempt in $(seq 1 60); do
  FLOWIE_RUNTIME_HEALTH="$(docker inspect --format '{{.State.Health.Status}}' \
    "$FLOWIE_RUNTIME_CONTAINER")"
  test "$FLOWIE_RUNTIME_HEALTH" = healthy && break
  sleep 1
done
test "$FLOWIE_RUNTIME_HEALTH" = healthy

FLOWIE_RUNTIME_TOPIC="flowie/runtime/${RUN_ID}"
FLOWIE_RUNTIME_MESSAGE="flowie-runtime-${RUN_ID}"
timeout 15 mosquitto_sub -h 127.0.0.1 -p "$FLOWIE_RUNTIME_PORT" -V mqttv5 \
  -q 1 -t "$FLOWIE_RUNTIME_TOPIC" -C 1 \
  > "$ARTIFACT_ROOT/flowie-server-runtime-message.txt" &
FLOWIE_SUB_PID=$!
sleep 1
mosquitto_pub -h 127.0.0.1 -p "$FLOWIE_RUNTIME_PORT" -V mqttv5 \
  -q 1 -t "$FLOWIE_RUNTIME_TOPIC" -m "$FLOWIE_RUNTIME_MESSAGE"
wait "$FLOWIE_SUB_PID"
grep -Fx "$FLOWIE_RUNTIME_MESSAGE" "$ARTIFACT_ROOT/flowie-server-runtime-message.txt"

docker stop --time 30 "$FLOWIE_RUNTIME_CONTAINER" >/dev/null
test "$(docker inspect --format '{{.State.ExitCode}}' "$FLOWIE_RUNTIME_CONTAINER")" -eq 0
docker logs "$FLOWIE_RUNTIME_CONTAINER" \
  > "$ARTIFACT_ROOT/flowie-server-runtime.log" 2>&1
printf 'health=%s\nmqtt5_qos1_pubsub=pass\ngraceful_exit=0\n' \
  "$FLOWIE_RUNTIME_HEALTH" \
  | tee "$ARTIFACT_ROOT/flowie-server-runtime.txt"
cleanup_flowie_runtime
trap - EXIT

docker image inspect "$FLOWIE_SERVER_IMAGE" \
  > "$ARTIFACT_ROOT/flowie-server-image.json"
printf 'image=%s\nrevision=%s\nimage_id=%s\n' \
  "$FLOWIE_SERVER_IMAGE" "$IMAGE_REVISION" \
  "$(docker image inspect --format '{{.Id}}' "$FLOWIE_SERVER_IMAGE")" \
  | tee "$ARTIFACT_ROOT/flowie-server-image.txt"
```

### 4.1 Debug 原生观测版本

需要观察 CoroNet/Flowie source、组件和资源边界时，从同一份八仓库源码构建完整 Debug image。三个 profile
参数必须一起设置；Debug 和 Release 使用不同 tag，不覆盖 release gate 镜像：

```bash
FLOWIE_DEBUG_IMAGE="flowie-server:debug-${RUN_ID,,}"
docker buildx build \
  --file "$FLOWIE_SRC/deploy/server/Dockerfile" \
  --build-context "turbo_utils=$TURBO_UTILS_SRC" \
  --build-context "turbo_parser=$TURBO_PARSER_SRC" \
  --build-context "turbo_net=$TURBO_NET_SRC" \
  --build-context "turbo_db=$TURBO_DB_SRC" \
  --build-context "turbo_http=$TURBO_HTTP_SRC" \
  --build-context "flow_mq=$FLOW_MQ_SRC" \
  --build-context "turbo_raft=$TURBO_RAFT_SRC" \
  --build-arg "SOURCE_REVISION=$SOURCE_REVISION" \
  --build-arg FLOWIE_BUILD_PRESET=linux-dev-user \
  --build-arg FLOWIE_INSTALL_PRESET=install-linux-dev-user \
  --build-arg FLOWIE_PROFILE=debug \
  --build-arg FLOWIE_ENABLE_ASAN=OFF \
  --tag "$FLOWIE_DEBUG_IMAGE" --progress=plain --load "$FLOWIE_SRC" \
  2>&1 | tee "$ARTIFACT_ROOT/flowie-server-debug-image-build.log"

# 在提取/运行镜像前执行 CoroNet 日志门禁。cacheonly 只保留构建缓存，不把包含
# toolchain 和测试二进制的 stage 导入本机镜像列表。
docker buildx build \
  --file "$FLOWIE_SRC/deploy/server/Dockerfile" \
  --build-context "turbo_utils=$TURBO_UTILS_SRC" \
  --build-context "turbo_parser=$TURBO_PARSER_SRC" \
  --build-context "turbo_net=$TURBO_NET_SRC" \
  --build-context "turbo_db=$TURBO_DB_SRC" \
  --build-context "turbo_http=$TURBO_HTTP_SRC" \
  --build-context "flow_mq=$FLOW_MQ_SRC" \
  --build-context "turbo_raft=$TURBO_RAFT_SRC" \
  --build-arg FLOWIE_BUILD_PRESET=linux-dev-user \
  --build-arg FLOWIE_INSTALL_PRESET=install-linux-dev-user \
  --build-arg FLOWIE_PROFILE=debug \
  --build-arg FLOWIE_ENABLE_ASAN=OFF \
  --target turbonet_logging_tests --output type=cacheonly \
  --progress=plain "$FLOWIE_SRC" \
  2>&1 | tee "$ARTIFACT_ROOT/coronet-logging-tests.log"

DEBUG_ROOT="$RUN_ROOT/native-flowie-debug"
install -d -m 0700 "$DEBUG_ROOT/bin" "$DEBUG_ROOT/lib" "$DEBUG_ROOT/artifacts"
DEBUG_CONTAINER="$(docker create "$FLOWIE_DEBUG_IMAGE")"
docker cp "$DEBUG_CONTAINER:/usr/local/bin/flowie_server" "$DEBUG_ROOT/bin/flowie_server"
docker cp "$DEBUG_CONTAINER:/usr/local/lib/." "$DEBUG_ROOT/lib/"
docker rm "$DEBUG_CONTAINER"

env LD_LIBRARY_PATH="$DEBUG_ROOT/lib" \
  "$DEBUG_ROOT/bin/flowie_server" --check --log-level DEBUG \
    --max-packet-size 1048576 --max-connections 2048 --max-sessions 2048 \
    --max-subscriptions-per-session 1024 --max-inflight 4096 \
    --max-retained-messages 2048 --send-hwm-bytes 1048576 \
    --coroutine-stack-size 65536 --stream-recv-buffer-bytes 4096 \
    --socket-recv-buffer-bytes 0 --socket-send-buffer-bytes 0 \
    --timeout-ms 0 --recv-timeout-ms 0 --tcp-keepalive \
    --tcp-keepalive-idle-ms 60000 --tcp-keepalive-interval-ms 10000 \
    --tcp-keepalive-count 3 \
  > "$DEBUG_ROOT/artifacts/check.log" 2>&1
grep -F '[Flowie.Server]' "$DEBUG_ROOT/artifacts/check.log"
grep -F 'max_inflight_per_session=4096' "$DEBUG_ROOT/artifacts/check.log"
grep -F 'coroutine_stack_size=65536' "$DEBUG_ROOT/artifacts/check.log"
grep -F 'tcp_keepalive=1' "$DEBUG_ROOT/artifacts/check.log"
```

原生运行时使用空闲 loopback 端口，并把 stdout/stderr 一起保存。先发送 SIGTERM 等待正常退出，再替换现有
run-scoped Flowie 进程；不得按名称批量 kill，也不得停止数据库或 Nginx：

```bash
env LD_LIBRARY_PATH="$DEBUG_ROOT/lib" \
  "$DEBUG_ROOT/bin/flowie_server" \
    --host 127.0.0.1 --port 18886 --transport tcp \
    --max-connections 256 --max-inflight 1024 --log-level DEBUG \
  > "$DEBUG_ROOT/artifacts/runtime.log" 2>&1 &
FLOWIE_DEBUG_PID=$!
printf '%s\n' "$FLOWIE_DEBUG_PID" > "$DEBUG_ROOT/flowie-server.pid"
```

负载后按结构化字段检查，而不是只看肉眼尾部：

```bash
grep -c '\[DEBUG\]' "$DEBUG_ROOT/artifacts/runtime.log" || true
grep -c '\[WARN\]' "$DEBUG_ROOT/artifacts/runtime.log" || true
grep -F '[CoroNet.epoll]' "$DEBUG_ROOT/artifacts/runtime.log" | tail -n 20
grep -F '[Flowie.Endpoint]' "$DEBUG_ROOT/artifacts/runtime.log" | tail -n 20
! grep -Eai 'password|authorization|api[_-]?key|secret|token|client_id|username|payload=' \
  "$DEBUG_ROOT/artifacts/runtime.log"
```

日志门禁通过真实 loopback socket 验证 FIN/RST，通过 fd exhaustion 注入 accept/EMFILE，并用只链接到测试
程序的 syscall wrapper 注入 epoll_wait/EBADF；不会向 CoroNet 生产库增加故障注入接口。每个异常必须恰好
一条 ERROR，并包含 operation/status/reason/action；accept 致命错误还必须先 disarm listener，避免 epoll
持续唤醒形成日志风暴。

`status=-4095` 是 `TURBO_EOF` 的正常对端关闭，不应逐连接记录；异常 read 在缓冲数据交付完成后由
`CoroNet.epoll` 记录一次 `stream-terminal operation/status/reason/action`。slow-subscriber WARN 应包含
`total_disconnects`、inflight/queue/HWM 上限和 action，不包含 MQTT 身份与内容字段。
库默认 `FLOWIE_DEFAULT_MAX_INFLIGHT_PER_SESSION` 是 `64`；上面的并发诊断显式使用 `1024`，避免 8 个
subscriber × 8 个 publisher × 25 条消息的突发先耗尽 session 容量。QoS 2 窗口在 `PUBCOMP` 后释放，
`PUBREL` 会稳定地排在尚未发送的 QoS PUBLISH 前且保持 FIFO。`qos2-window` DEBUG 每连接最多记录一次
满窗和一次释放，不能随消息数线性增长。

Debug 运行只用于限定时间的诊断，不代表适合长期保留。压测后还必须按
`docs/FLOWIE_LONG_RUNNING_LOGGING.md` 检查事件所有权、模板数量和字节预算。尤其不能把正常 EOF、正常 close
和主动 shutdown 在 transport/session 多层重复记录；生产 INFO 及以上的正常连接生命周期预算为 0 行。

### 4.2 多客户端 fan-out 压测与内容校验

多客户端压测使用独立的 loopback listener，不替换上一节的诊断进程，也不操作 PostgreSQL/Nginx 容器。
这是当前且唯一的 Flowie 负载验证入口。Debug 结果用于验证正确性和资源边界，
不能直接作为稳定的吞吐、时延或生产容量 SLA。
runner 的 payload 是一行 ASCII 校验记录：
`run=<id> qos=<qos> publisher=<n> sequence=<n> marker=flowie-scale-v1`。未设置 `--payload-bytes` 时长度随编号
变化，典型运行约为 89--91 bytes；设置后，runner 用 ASCII `x` 把 marker 字段补齐到精确的 100--4096
bytes，并在每个 subscriber 上校验总长度和全部 padding。payload 大小不包含 MQTT topic、固定头和 QoS
packet id。publisher 没有固定间隔或目标速率；全部客户端就绪后由同一个 gate 同时释放，每个
`mosquitto_pub -l` 以可达到的速度连续发送。因此 summary 中的 `deliveries_per_second` 和
`delivery_payload_bytes_per_second` 是所有 subscriber 完成内容校验的端到端 fan-out 吞吐，不是单
publisher 的发送限速。
默认三级负载中的 `N` 同时表示每个 QoS 的 subscriber 数和 publisher 数：

| N | 总客户端 `3 × (N + N)` | 每个 QoS 发布数 `N × 25` | 每个 QoS fan-out 投递数 `N × N × 25` | 三个 QoS 总投递数 |
|---:|---:|---:|---:|---:|
| 16 | 96 | 400 | 6,400 | 19,200 |
| 32 | 192 | 800 | 25,600 | 76,800 |
| 64 | 384 | 1,600 | 102,400 | 307,200 |
| 96 | 576 | 2,400 | 230,400 | 691,200 |
| 128 | 768 | 3,200 | 409,600 | 1,228,800 |

容量前置条件为 `max_connections >= 6N`、`max_inflight_per_session >= N × messages`。payload 数据量为
`3 × N × N × messages × payload_bytes`，runner 对这个乘积执行 signed-64-bit overflow 检查。`N=128`、
25 条的客户端扩展档需要至少 768 个连接和 3,200 条 per-session inflight；诊断 listener 使用
`2048/4096` 留出余量。这些公式只证明测试输入不超过显式配额，不代表吞吐能力或生产容量。不要未经预算
直接组合所有最大维度：`N=128`、50 条、4096 bytes 会产生 10,066,329,600 bytes 的 fan-out payload。

推荐把客户端数和 payload 大小拆成有界矩阵；最后一行同时覆盖较多客户端与 4 KiB payload，但降低消息数：

| 目的 | N | 总客户端 | messages | payload bytes | 总投递 | delivered payload bytes |
|---|---:|---:|---:|---:|---:|---:|
| payload sweep | 32 | 192 | 25 | 100/256/1024/4096 | 76,800 | 7,680,000--314,572,800 |
| client sweep | 64/96/128 | 384/576/768 | 25 | 100 | 307,200--1,228,800 | 30,720,000--122,880,000 |
| combined | 96 | 576 | 5 | 4096 | 138,240 | 566,231,040 |

```bash
FLOWIE_SCALE_PORT=18890
FLOWIE_SCALE_MAX_PACKET_SIZE=1048576
FLOWIE_SCALE_MAX_CONNECTIONS=2048
FLOWIE_SCALE_MAX_SESSIONS=2048
FLOWIE_SCALE_MAX_SUBSCRIPTIONS=1024
FLOWIE_SCALE_MAX_INFLIGHT=4096
FLOWIE_SCALE_MAX_RETAINED=2048
FLOWIE_SCALE_SEND_HWM=1048576
FLOWIE_SCALE_COROUTINE_STACK=65536
FLOWIE_SCALE_STREAM_RECV_BUFFER=4096
FLOWIE_SCALE_ROOT="$RUN_ROOT/native-flowie-scale-debug"
FLOWIE_SCALE_SESSION="flowie-scale-debug-${RUN_ID,,}"
install -d -m 0700 "$FLOWIE_SCALE_ROOT/artifacts"

if ss -Hltn | awk -v suffix=":$FLOWIE_SCALE_PORT" '$4 ~ (suffix "$") { found=1 } END { exit !found }'; then
  echo "port already in use: $FLOWIE_SCALE_PORT" >&2
  exit 1
fi
tmux new-session -d -s "$FLOWIE_SCALE_SESSION" \
  "exec env LD_LIBRARY_PATH=$DEBUG_ROOT/lib $DEBUG_ROOT/bin/flowie_server \
    --host 127.0.0.1 --port $FLOWIE_SCALE_PORT --transport tcp \
    --max-packet-size $FLOWIE_SCALE_MAX_PACKET_SIZE \
    --max-connections $FLOWIE_SCALE_MAX_CONNECTIONS \
    --max-sessions $FLOWIE_SCALE_MAX_SESSIONS \
    --max-subscriptions-per-session $FLOWIE_SCALE_MAX_SUBSCRIPTIONS \
    --max-inflight $FLOWIE_SCALE_MAX_INFLIGHT \
    --max-retained-messages $FLOWIE_SCALE_MAX_RETAINED \
    --send-hwm-bytes $FLOWIE_SCALE_SEND_HWM \
    --coroutine-stack-size $FLOWIE_SCALE_COROUTINE_STACK \
    --stream-recv-buffer-bytes $FLOWIE_SCALE_STREAM_RECV_BUFFER \
    --socket-recv-buffer-bytes 0 --socket-send-buffer-bytes 0 \
    --timeout-ms 0 --recv-timeout-ms 0 --tcp-keepalive \
    --tcp-keepalive-idle-ms 60000 --tcp-keepalive-interval-ms 10000 \
    --tcp-keepalive-count 3 --log-level DEBUG \
    >> $FLOWIE_SCALE_ROOT/artifacts/runtime.log 2>&1"
sleep 2
FLOWIE_SCALE_PID="$(tmux display-message -p -t "$FLOWIE_SCALE_SESSION" '#{pane_pid}')"
printf '%s\n' "$FLOWIE_SCALE_PID" > "$FLOWIE_SCALE_ROOT/flowie-server.pid"
kill -0 "$FLOWIE_SCALE_PID"

FLOWIE_SCALE_RESULTS="$RUN_ROOT/artifacts/mqtt-scale"
bash "$FLOWIE_SRC/deploy/server/tests/run-mqtt-scale-load.sh" \
  --host 127.0.0.1 --port "$FLOWIE_SCALE_PORT" \
  --server-pid "$FLOWIE_SCALE_PID" \
  --server-log "$FLOWIE_SCALE_ROOT/artifacts/runtime.log" \
  --artifacts "$FLOWIE_SCALE_RESULTS" \
  --max-connections "$FLOWIE_SCALE_MAX_CONNECTIONS" \
  --max-inflight "$FLOWIE_SCALE_MAX_INFLIGHT" \
  --tiers 16,32,64 --messages 25 --payload-bytes 100 --timeout 300
```

`send_hwm_bytes` 必须按单 subscriber 的未发送 burst 预算，而不是只按单条 payload 预算。100-byte client
sweep 可保留上例的 1 MiB；`N=32`、25 条、4096-byte 的单 subscriber payload 上界已经约为
`32 × 25 × 4096 = 3,276,800` bytes，另有 MQTT topic/header 和队列元数据，因此使用独立 listener/profile
并设置 `FLOWIE_SCALE_SEND_HWM=4194304`。1 MiB HWM 会在 queued replies 约 232 条时按设计触发
slow-subscriber isolation；这属于容量配置失败，不能作为 4 KiB 传输成功。也不要把 4 MiB 无条件用于全部
2048 个连接：理论 per-connection 预算乘积为 8 GiB，必须按实际矩阵和主机内存控制并发。

这些参数都是启动时配置，修改后必须重启 listener。`max_connections` 是并发连接边界，`max_sessions` 是受管
session 总量；`max_subscriptions_per_session` 和 `max_inflight` 是单 session 边界，retained 是 endpoint
总量。这里把 coroutine stack 从组件默认的 128 KiB 显式降为 64 KiB，并保留 4 KiB stream receive
chunk，以降低 Debug 高并发的私有 pool 上界；上线前仍须针对实际调用深度运行 ASan/长时压力测试。
`send_hwm_bytes` 是每连接待发送预算，达到上限会触发慢订阅者隔离，不应仅通过不断放大 HWM 掩盖下游
拥塞。OS socket buffer 保持 `0`，让内核采用主机默认值；keepalive 用于清理失联 TCP peer。
独立 `flowie_server` 不创建 worker，`reuse-port` 因此不用于本单 listener 压测；worker 数只能在完整 YAML
supervisor runtime 中配置。

runner 先启动所有 subscriber，再让 publisher 建立 MQTT 连接并在 barrier 等待；只有已建连接数连续 10 次
以 100 ms 间隔达到该档 `6N` 后才统一发送。每条内容使用唯一 run namespace 和确定字段
`run/qos/publisher/sequence/marker`，不含身份、凭据或生产 payload。每个 subscriber 必须同时通过：

1. Mosquitto 进程在 timeout 内以 0 退出，收到精确的 `N × messages` 行；
2. 实际内容排序后与期望 multiset 完全相同，以检出缺失、重复、跨 run 和内容损坏；
3. 每个 publisher 的 sequence 独立保持 `1..messages`，不要求不同 publisher 之间的全局顺序；显式
   payload 大小时，每行长度必须完全相等且 padding 只能包含 `x`；
4. 服务 PID/listener 保持存活，采样到的连接峰值不低于 `6N`；
5. subscriber 显式声明 Receive Maximum=20；默认三档的单 subscriber 投递量都超过该窗口。日志增量无
   WARN/ERROR/FATAL、空 component/source 或敏感字段名，`qos2-window` 恰好为 `2N`，
   同时证明每个 QoS2 subscriber 各产生一次满窗和释放采样且这些关键 DEBUG 未丢失。

QoS 0 在协议上仍是 at-most-once；这里在无故障 loopback 环境使用严格零丢失 oracle。QoS 1/2 在本用例中
没有重连或 retransmit 注入，也要求精确一次内容集合。`summary.csv` 保留原字段顺序，并在 `result` 后追加
`payload_bytes`、`expected_delivery_payload_bytes` 和 `delivery_payload_bytes_per_second`；同时记录 elapsed、
delivery rate、CPU、RSS/VM、thread、FD、连接峰值和日志计数。单次 RSS settled 值只作为观测证据。私有 CoroNet coroutine
pool 的容量上限是 `2 × max_connections + 32`，默认 coroutine stack 是 128 KiB；release 会把 coroutine
放回 free-list 复用而不是立即销毁。因此高并发后的 RSS 高水位可能保留到 endpoint shutdown。以本节的
2,048 连接配置计算，若 pool 完全增长，仅 coroutine stack 的容量上界约为
`(2 × 2048 + 32) × 128 KiB = 516 MiB`，还不含连接和协议状态。重复较小负载不应在已经达到的高水位之上
持续阶梯增长；内存回归阈值仍要在稳定主机至少运行 10 次建立中位数与离散度后固化，不能由一次 Debug
运行猜测。

这里使用源码归档 SHA-256 作为 `org.opencontainers.image.revision`，因为归档包含未提交工作树，Git commit
不能唯一表示实际镜像输入。发布 CI 若只接受已提交源码，则应改用八仓库 manifest 和实际 Flowie commit、将 `--load`
替换为 `--push`，记录 registry digest，并让部署引用不可变 digest。

## 5. 启动固定 Mosquitto

EU 主机的 `8883` 由现有 Nginx 占用，因此固定互操作 broker 使用本次 run 专用的高位宿主端口；
容器内端口仍为 Mosquitto 标准端口。先确认这些测试端口没有被其他服务占用，发生冲突时停止并确认
占用者，不自动复用或停止未知服务。

```bash
export FLOWIE_MQTT_FIXED_TCP_PORT=11883
export FLOWIE_MQTT_FIXED_TLS_PORT=18884
export FLOWIE_MQTT_FIXED_WS_PORT=18083
export FLOWIE_MQTT_FIXED_WSS_PORT=18084

for port in \
  "$FLOWIE_MQTT_FIXED_TCP_PORT" "$FLOWIE_MQTT_FIXED_TLS_PORT" \
  "$FLOWIE_MQTT_FIXED_WS_PORT" "$FLOWIE_MQTT_FIXED_WSS_PORT"; do
  if ss -ltnH | awk '{print $4}' | grep -Eq "[:.]${port}$"; then
    echo "port already in use: $port" >&2
    exit 1
  fi
done

CERT_DIR="$RUN_ROOT/certs/mosquitto"
install -d -m 0755 "$CERT_DIR"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
  -subj '/CN=Flowie MQTT Test CA' \
  -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.pem"
openssl req -newkey rsa:2048 -nodes -subj '/CN=localhost' \
  -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.csr"
openssl x509 -req -days 2 -in "$CERT_DIR/server.csr" \
  -CA "$CERT_DIR/ca.pem" -CAkey "$CERT_DIR/ca.key" -CAcreateserial \
  -out "$CERT_DIR/server.pem" \
  -extfile "$FLOWIE_SRC/interop/mosquitto-2.0.22/server.ext"
chmod 0644 "$CERT_DIR/ca.pem" "$CERT_DIR/server.pem" "$CERT_DIR/server.key"

export FLOWIE_MQTT_INTEROP_CERT_DIR="$CERT_DIR"
export COMPOSE_PROJECT_NAME="flowie${RUN_ID,,}"
docker compose -f "$FLOWIE_SRC/interop/mosquitto-2.0.22/compose.yml" \
  up -d --wait

docker compose -f "$FLOWIE_SRC/interop/mosquitto-2.0.22/compose.yml" \
  ps --format json > "$ARTIFACT_ROOT/docker-containers.json"
docker image inspect eclipse-mosquitto:2.0.22 \
  > "$ARTIFACT_ROOT/docker-images.json"
```

仅在显式启用可选 public MQTT smoke 时检查以下出站端口；失败不影响 release gate：

```bash
if [[ "${FLOWIE_RUN_PUBLIC_SMOKE:-0}" == "1" ]]; then
  for endpoint in \
    broker.hivemq.com:1883 broker.hivemq.com:8000 \
    broker.emqx.io:1883 broker.emqx.io:8883 broker.emqx.io:8083 broker.emqx.io:8084; do
    host="${endpoint%:*}"
    port="${endpoint##*:}"
    timeout 5 bash -c "</dev/tcp/$host/$port"
  done
fi
```

## 6. 构建、测试并安装依赖 SDK

顺序固定为 TurboUtils → TurboParser → TurboNet → TurboDB → TurboHTTP → FlowMQ → TurboRaft。
宿主机测试使用独立的 `build/linux-eu-release` 构建树，并把 SDK 安装到 `$SDK_ROOT/<package>/release`。
这里直接给 CMake 传入路径，不使用各仓库面向镜像构建的 `/opt` Linux user preset 环境。

```bash
configure_build_test_install() {
  package="$1"
  source_dir="$2"
  install_dir="$3"
  shift 3
  build_dir="$source_dir/build/linux-eu-release"
  vcpkg_args=(
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake
    -DVCPKG_INSTALLED_DIR="$source_dir/vcpkg_installed"
    -DVCPKG_TARGET_TRIPLET=x64-linux
    -DVCPKG_MANIFEST_MODE=ON
  )
  if [[ -d "$source_dir/vcpkg-ports" ]]; then
    vcpkg_args+=("-DVCPKG_OVERLAY_PORTS=$source_dir/vcpkg-ports")
  elif [[ -d "$source_dir/vcpkg-overlays" ]]; then
    vcpkg_args+=("-DVCPKG_OVERLAY_PORTS=$source_dir/vcpkg-overlays")
  fi

  env "$@" cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    "${vcpkg_args[@]}" \
    -DCMAKE_INSTALL_PREFIX="$install_dir" \
    -DENABLE_TESTS=ON -DBUILD_TESTING=ON
  env "$@" cmake --build "$build_dir" --parallel "$(nproc)"
  env "$@" ctest --test-dir "$build_dir" --output-on-failure \
    --output-junit "$ARTIFACT_ROOT/$package-linux-release.xml"
  env "$@" cmake --install "$build_dir"
}

TU="$SDK_ROOT/turboutils/release"
TP="$SDK_ROOT/turboparser/release"
TN="$SDK_ROOT/turbonet/release"
TD="$SDK_ROOT/turbodb/release"
TH="$SDK_ROOT/turbohttp/release"
FM="$SDK_ROOT/flowmq/release"
TR="$SDK_ROOT/turboraft/release"

configure_build_test_install turboutils "$TURBO_UTILS_SRC" "$TU"
configure_build_test_install turboparser "$TURBO_PARSER_SRC" "$TP" \
  TURBOUTILS_ROOT="$TU"
configure_build_test_install turbonet "$TURBO_NET_SRC" "$TN" \
  TURBOUTILS_ROOT="$TU" TURBOPARSER_ROOT="$TP"
configure_build_test_install turbodb "$TURBO_DB_SRC" "$TD" \
  TURBOUTILS_ROOT="$TU" TURBOPARSER_ROOT="$TP" TURBONET_ROOT="$TN"
configure_build_test_install turbohttp "$TURBO_HTTP_SRC" "$TH" \
  TURBOUTILS_ROOT="$TU" TURBOPARSER_ROOT="$TP" TURBONET_ROOT="$TN" TURBODB_ROOT="$TD"
configure_build_test_install flowmq "$FLOW_MQ_SRC" "$FM" \
  TURBOUTILS_ROOT="$TU" TURBOPARSER_ROOT="$TP" TURBONET_ROOT="$TN" TURBODB_ROOT="$TD"
configure_build_test_install turboraft "$TURBO_RAFT_SRC" "$TR" \
  TURBOUTILS_ROOT="$TU" TURBOPARSER_ROOT="$TP" TURBONET_ROOT="$TN" \
  TURBOHTTP_ROOT="$TH" FLOWMQ_ROOT="$FM"

export LD_LIBRARY_PATH="$TR/lib:$FM/lib:$TH/lib:$TD/lib:$TN/lib:$TP/lib:$TU/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

`LD_LIBRARY_PATH` 必须保持本次 run 的 SDK 在主机系统路径之前，避免
`/usr/local/lib` 中旧版本库满足同名 SONAME 后造成 ABI 符号错配。

## 7. 配置并构建 Flowie release gate

```bash
cd "$FLOWIE_SRC"
FLOWIE_RELEASE_BUILD="$FLOWIE_SRC/build/linux-eu-release"
FLOWIE_CONTROL_PGSQL_ARGS=()
if [[ "${FLOWIE_RUN_CONTROL_PGSQL_LIVE:-0}" == "1" ]]; then
  test -n "${TURBO_FLOW_PGSQL_TEST_CONNINFO:-}"
  FLOWIE_CONTROL_PGSQL_ARGS+=(
    -DFLOWIE_CONTROL_PGSQL=ON
    -DTURBO_FLOW_PGSQL_LIVE_TESTS=ON
  )
fi

env \
  TURBOUTILS_ROOT="$TU" TURBOPARSER_ROOT="$TP" TURBONET_ROOT="$TN" \
  TURBODB_ROOT="$TD" TURBOHTTP_ROOT="$TH" FLOWMQ_ROOT="$FM" TURBORAFT_ROOT="$TR" \
cmake -S "$FLOWIE_SRC" -B "$FLOWIE_RELEASE_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_INSTALLED_DIR="$FLOWIE_SRC/vcpkg_installed" \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DVCPKG_MANIFEST_MODE=ON \
  -DCMAKE_INSTALL_PREFIX="$SDK_ROOT/flowie/release" \
  -DFLOWIE_MQTT_RELEASE_GATE=ON \
  -DFLOWIE_MQTT_PUBLIC_LIVE_TESTS=OFF \
  -DFLOWIE_MQTT_FIXED_INTEROP_TESTS=ON \
  -DFLOWIE_MQTT_FIXED_CA_FILE="$CERT_DIR/ca.pem" \
  -DFLOWIE_MQTT_FIXED_TCP_PORT="$FLOWIE_MQTT_FIXED_TCP_PORT" \
  -DFLOWIE_MQTT_FIXED_TLS_PORT="$FLOWIE_MQTT_FIXED_TLS_PORT" \
  -DFLOWIE_MQTT_FIXED_WS_PORT="$FLOWIE_MQTT_FIXED_WS_PORT" \
  -DFLOWIE_MQTT_FIXED_WSS_PORT="$FLOWIE_MQTT_FIXED_WSS_PORT" \
  -DFLOWIE_MQTT_FIXED_SUPPORT_31=ON \
  -DFLOWIE_MQTT_FIXED_SUPPORT_31_WS=ON \
  -DFLOWIE_MQTT_SOAK_TESTS=ON \
  -DFLOWIE_MQTT_FUZZ_TARGETS=OFF \
  -DFLOWIE_RELEASE_REVISION="$SOURCE_REVISION" \
  "${FLOWIE_CONTROL_PGSQL_ARGS[@]}" \
  -DENABLE_TESTS=ON -DBUILD_TESTING=ON

cmake --build "$FLOWIE_RELEASE_BUILD" --parallel "$(nproc)" \
  2>&1 | tee "$ARTIFACT_ROOT/flowie-linux-release-build.log"
```

## 8. 分层运行 cases

以下顺序先验证外部依赖，再运行 Flowie release label、全量 CTest 和 release evidence。任何一步失败即
停止，不继续用后续结果掩盖失败。

```bash
cd "$FLOWIE_SRC"

ctest --test-dir "$FLOWIE_RELEASE_BUILD" -N \
  2>&1 | tee "$ARTIFACT_ROOT/flowie-linux-tests.txt"

ctest --test-dir "$FLOWIE_RELEASE_BUILD" --output-on-failure \
  -R '^(test_flowie_protocol_repository|test_flowie_cluster_raft_store|test_flowie_cluster_state_machine)$' \
  --output-junit "$ARTIFACT_ROOT/persistence.xml"

if [[ "${FLOWIE_RUN_CONTROL_PGSQL_LIVE:-0}" == "1" ]]; then
  ctest --test-dir "$FLOWIE_RELEASE_BUILD" --output-on-failure \
    -R '^(test_flowie_protocol_repository_pgsql_live|test_flowie_control_pgsql_database(_live)?)$' \
    --output-junit "$ARTIFACT_ROOT/control-postgresql.xml"
fi

ctest --test-dir "$FLOWIE_RELEASE_BUILD" --output-on-failure \
  -L 'mqtt-fixed-interop' \
  --output-junit "$ARTIFACT_ROOT/fixed-interop.xml"

ctest --test-dir "$FLOWIE_RELEASE_BUILD" --output-on-failure \
  -L 'flowie-release' \
  --output-junit "$ARTIFACT_ROOT/flowie-release.xml"

ctest --test-dir "$FLOWIE_RELEASE_BUILD" --output-on-failure \
  --output-junit "$ARTIFACT_ROOT/flowie-linux-release.xml"

cmake --build "$FLOWIE_RELEASE_BUILD" --target flowie_release_evidence \
  2>&1 | tee "$ARTIFACT_ROOT/flowie-release-evidence.log"

test -f "$FLOWIE_RELEASE_BUILD/flowie-release-evidence.json"
cp "$FLOWIE_RELEASE_BUILD/flowie-release-evidence.json" "$ARTIFACT_ROOT/"
cmake --install "$FLOWIE_RELEASE_BUILD"
```

本阶段的 `test_flowie_mqtt_soak` 使用默认短时验证。规定的 30/60 分钟 soak 只由第 9 节 nightly
evidence 生成。

## 9. Clang sanitizer、fuzz 与 30/60 分钟 nightly

本节约需四小时，应保持在 `tmux` 中。它使用独立 Clang 构建树；不能在
GCC release build 中开启 `FLOWIE_MQTT_FUZZ_TARGETS`。

```bash
cd "$FLOWIE_SRC"
FLOWIE_NIGHTLY_BUILD="$FLOWIE_SRC/build/linux-eu-clang-debug"

env \
  TURBOUTILS_ROOT="$TU" TURBOPARSER_ROOT="$TP" TURBONET_ROOT="$TN" \
  TURBODB_ROOT="$TD" TURBOHTTP_ROOT="$TH" FLOWMQ_ROOT="$FM" TURBORAFT_ROOT="$TR" \
cmake -S "$FLOWIE_SRC" -B "$FLOWIE_NIGHTLY_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_INSTALLED_DIR="$FLOWIE_SRC/vcpkg_installed" \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DVCPKG_MANIFEST_MODE=ON \
  -DCMAKE_INSTALL_PREFIX="$SDK_ROOT/flowie/clang-debug" \
  -DENABLE_TESTS=ON -DBUILD_TESTING=ON \
  -DENABLE_SANITIZER_ADDRESS=ON \
  -DFLOWIE_MQTT_RELEASE_GATE=OFF \
  -DFLOWIE_MQTT_PUBLIC_LIVE_TESTS=OFF \
  -DFLOWIE_MQTT_FIXED_INTEROP_TESTS=OFF \
  -DFLOWIE_MQTT_SOAK_TESTS=ON \
  -DFLOWIE_MQTT_FUZZ_TARGETS=ON \
  -DFLOWIE_RELEASE_REVISION="$SOURCE_REVISION"

cmake --build "$FLOWIE_NIGHTLY_BUILD" --target flowie_nightly_evidence \
  --parallel "$(nproc)" 2>&1 | tee "$ARTIFACT_ROOT/flowie-nightly-evidence.log"

test -f "$FLOWIE_NIGHTLY_BUILD/flowie-nightly-evidence.json"
cp "$FLOWIE_NIGHTLY_BUILD/flowie-nightly-evidence.json" "$ARTIFACT_ROOT/"
cp -a "$FLOWIE_NIGHTLY_BUILD/flowie-fuzz-artifacts" "$ARTIFACT_ROOT/"
```

nightly target 固定要求：同一 `SOURCE_REVISION`、非零 seed、corpus/soak/sanitizer 全部 PASS、六项
`resource_monotonic_growth=false`。任一条件缺失会由 verifier 直接失败。

## 10. 收集结果并下载

```bash
cd "$RUN_ROOT"
docker compose -f "$FLOWIE_SRC/interop/mosquitto-2.0.22/compose.yml" \
  logs --no-color > "$ARTIFACT_ROOT/mosquitto.log" 2>&1

RESULT_NAME="flowie-linux-results-$RUN_ID.zip"
zip -qr "/root/dev/incoming/$RESULT_NAME" artifacts
sha256sum "/root/dev/incoming/$RESULT_NAME" \
  > "/root/dev/incoming/$RESULT_NAME.sha256"
printf '%s\n' "$RESULT_NAME" > /root/dev/incoming/latest-result
```

在 Windows PowerShell 下载并验证：

```powershell
$resultName = ssh root@eu 'cat /root/dev/incoming/latest-result'
$downloadRoot = 'C:\projects\cpp\artifacts\eu-results'
New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
scp "root@eu:/root/dev/incoming/$resultName" "$downloadRoot/"
scp "root@eu:/root/dev/incoming/$resultName.sha256" "$downloadRoot/"
Push-Location $downloadRoot
try {
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $resultName).Hash.ToLowerInvariant()
    $expected = ((Get-Content -Raw -LiteralPath "$resultName.sha256").Trim() -split '\s+')[0]
    if ($actual -ne $expected) { throw "result checksum mismatch: $resultName" }
    Write-Host "verified=$resultName sha256=$actual"
} finally {
    Pop-Location
}
```

## 11. 精确清理 Docker 资源

只在日志与结果包生成后执行。以下保护确保仅删除本次 Flowie/Mosquitto compose project 和本次 Flowie
镜像；源码、SDK 和证据仍保留在 `$RUN_ROOT`，已有 PostgreSQL、Redis、Nginx 容器及其卷均不在清理范围。

```bash
case "$COMPOSE_PROJECT_NAME" in flowie*) ;; *) exit 1 ;; esac
case "$FLOWIE_SERVER_IMAGE" in flowie-server:run-*) ;; *) exit 1 ;; esac

docker compose -f "$FLOWIE_SRC/interop/mosquitto-2.0.22/compose.yml" \
  down -v
docker image rm "$FLOWIE_SERVER_IMAGE"
```

## 12. 成功判定

一次完整 Linux 结果必须同时满足：

- TurboUtils、TurboParser、TurboNet、TurboDB、TurboHTTP、FlowMQ、TurboRaft 与 Flowie configure/build 成功。
- 七个依赖 JUnit 与 Flowie release JUnit 结果无失败，Flowie 全量 CTest 不是零用例。
- TCP/TLS/WS/WSS 的 Flowie 端到端用例均有实际 PASS，固定 Mosquitto 与 TLS/WSS/mTLS 证据均完整；
  UDP/Unix Pipe 不属于当前成功条件。若本次启用 Redis/PostgreSQL live gate，对应结果也必须 PASS。
- `flowie-release-evidence.json` 通过内置 verifier。
- `deploy/server/Dockerfile` 从本次八仓库源码构建成功；镜像 revision 等于源码归档 SHA-256，
  runtime 用户和动态库验证通过；`flowie_server --help` 输出 usage，且符合当前 TurboUtils parser 的
  退出码 `1` 契约；镜像默认入口达到 `healthy`，MQTT 5 QoS 1 收发成功，SIGTERM 退出码为 `0`。
- nightly 的 corpus、六项 30/60 分钟 soak 和 Clang libFuzzer 全部 PASS，且无资源单调增长。
- 结果记录同一个源码归档 SHA-256；Linux 结果不得从 Windows 结果推定。

失败时保留 `$RUN_ROOT`、容器日志、JUnit、evidence JSON、seed 和 fuzz artifacts。先运行失败输出中给出的
单个 CTest 名称或 TinyTest filter 复现；确认根因后重新打包新的源码归档，不在远端源码上做无记录修改。
