# Flowie 单容器部署设计

## 背景与目标

当前运行镜像已经同时包含 `flowie_server` 与 `flowie-control`，但 Compose 将二者部署为两个容器。目标是让
标准 Compose 部署只创建一个名为 `flowie-server` 的容器，并在该容器内同时运行 MQTT Broker 与 HTTPS
Control；镜像直接运行时的 Broker-only 默认行为继续兼容。

本设计不合并两个 executable，不改变 MQTT、HTTPS、Management RPC、认证、ACL 或 SQLite 数据格式，也
不把 Control bootstrap 密码写入镜像或环境变量。

## 候选方案

### 方案 A：容器入口脚本监督两个进程（选择）

新增 POSIX shell 入口，分别启动既有 Server 与 Control entrypoint。任一子进程退出时，入口脚本终止另一
子进程、等待资源释放并返回失败进程的退出码；收到 TERM/INT/HUP 时把信号转发给两个子进程并等待退出。

- 优点：复用现有参数、secret 与配置契约，不改变二进制或协议；不增加运行时依赖。
- 代价：Broker 与 Control 共用容器生命周期和健康状态，任一服务失败都会重启二者。

### 方案 B：使用 `flowie_supervisor`/embedded Control

现有 supervisor 能把 Control runtime 嵌入完整 Flowie YAML worker，但当前容器的 Broker 入口使用直接
listener 参数，不读取 config/graph。采用该方案会改变公开配置模型、构建产物和数据面启动路径，迁移成本
与回归面明显更大。

### 方案 C：引入 supervisord/s6

外部 supervisor 能管理多进程，但会新增包、配置语法、镜像体积与补丁维护面。两个固定子进程不需要这类
依赖。

## 运行与状态归属

- `flowie-combined-entrypoint` 是容器生命周期所有者；Server 与 Control 是其直接子进程。
- Broker 的 session、subscription、inflight、retained 与 pending Will 仍是 `flowie_server` 进程内状态。
- Control 的 Domain、用户、credential、Role、ACL 与审计仍以 SQLite Repository 为事实源。
- Compose 保留 `flowie-data:/var/lib/flowie`，并把 `flowie-control-data` 嵌套挂载到
  `/var/lib/flowie/control`，避免迁移或合并两个卷的数据。
- TLS 配置、证书和私钥继续只读挂载；私钥口令继续由 file-backed secret 注入。

## 启动、失败和关闭语义

1. `FLOWIE_COMBINED_CHECK=1` 时顺序执行 Server 与 Control 的完整参数/配置预检，不打开 listener。
2. 正常模式先启动 Server，再启动 Control；两个进程都成功存活时入口保持运行。
3. 任一进程退出，入口立即向仍存活的进程发送 TERM、等待其退出，并返回首个退出码；意外的零退出转换为
   `1`，避免 Docker 将不完整服务视为成功。
4. 容器收到 TERM、INT 或 HUP 时，入口向两个子进程转发对应信号并等待。Compose 的 30 秒 grace period
   继续作为强制终止边界。
5. 健康检查必须同时探测 MQTT 与 Control TCP listener；任一端口不可达即 unhealthy。

## 兼容性、迁移与回滚

- 镜像默认 `ENTRYPOINT` 仍为现有 `docker-entrypoint.sh`，直接 `docker run` 保持 Broker-only。
- Compose 显式覆盖为 `flowie-combined-entrypoint`，因此标准 Compose 从两个 service 变为一个 service。
- 运维命令需从 `flowie-control` service 改为 `flowie-server`；这是用户明确要求的部署方式变更。
- 升级会短暂中断 MQTT 与 Control。升级前停止 Control 写入并备份 Control SQLite 卷。
- 回滚时使用旧 Compose 文件/镜像重新创建两个 service，并复用原 `flowie-data` 与
  `flowie-control-data`；数据格式未改变，无需回滚数据库。

## 安全与验证

- 容器继续使用 UID/GID `10001:10001`、只读根文件系统、`no-new-privileges` 和 `cap_drop: ALL`。
- Control 仍默认监听 `127.0.0.1:8443`；bootstrap 密码仍只允许在 loopback/受保护隧道完成首次改密。
- 自动验证覆盖参数预检、双进程启动、子进程失败联动关闭、信号转发与双端口健康检查。
- 镜像验证覆盖 `ldd`、Compose config、单容器数量、容器重启、MQTT CONNECT/PING/DISCONNECT 与 HTTPS
  bootstrap 登录/强制改密跳转/退出。
