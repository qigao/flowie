# Chttp App 与共享 JWT 接入

## 背景与决定

事实：当前 Chttp 将 server 初始化、路由与 WebSocket 注册收为私有引擎入口。
Flowie 的控制面和 MQTT WebSocket 原来直接调用这些入口，无法消费当前 SDK。
用户选择迁到 `CHttp::App`，同时复用 `http-services` 中的 cjwt。

候选方案是迁到公开 App，或为 Flowie 重新导出独立引擎接口。
选择 App 后，网络 owner、worker、请求快照、deferred terminal 和关闭由 Chttp
统一管理。Flowie 保留 MQTT 状态、控制面领域处理与认证策略，避免两套线程池和
deferred 句柄各自推进同一次响应。代价是业务回调改在 App worker 上执行，
WebSocket 的 `on_open` 改在升级完成后的 ready 阶段触发。

## 所有权与失败边界

- 控制面 adapter 持有 App、借用 Bean、路由与 context，直到 App 完成关闭。
  worker 将请求复制为现有处理器需要的可变缓冲，供凭据清零和请求解析使用；
  Invocation 及其借用视图不逃出回调。响应通过复制后的 App draft 发布。
- 响应复制失败直接返回错误，由 App 收敛为失败响应并消费 deferred terminal；
  不在 Flowie 层保存或重试已转移的句柄。
- MQTT WebSocket 在 opening 阶段选择客户端实际提供的 `mqtt` 子协议，再显式
  接受升级。未提供该协议时返回 400；普通响应头接口仍禁止设置保留握手字段。
  ready 阶段分配 Flowie peer 并调用 `on_open`；此时资源失败会关闭已升级连接。
- 同一 peer 的 ready、message、close 回调由 App 串行执行；不同 peer 可并发。
  Flowie 的共享 peer 表仍受 mutex 保护，跨线程发送保留原有代次检查。
- 有期限的关闭失败保留 App 和借用对象，允许调用方继续关闭。外层销毁必须等
  worker 与网络 owner 结束后才能释放 Bean、路由和 peer。

## 配额与兼容性

控制面继续使用已有 worker 数和队列容量；App 总保留预算为 192 MiB，
单响应预算为 16 MiB 加 64 KiB，额外响应头最多 32 项、64 KiB。
这是硬上限：并发大响应可先耗尽字节预算，入队容量不承诺所有大响应同时驻留。

MQTT WebSocket 每 peer 的事件容量为 `max(1, event_capacity / connection_capacity)`。
事件存储至少容纳控制帧，否则按最大消息大小计费；输入缓冲为最大消息大小的
两倍加完整帧头。总 worker 保留预算
按连接容量乘以每 peer 的事件存储、请求头、目标路径和固定开销计算；乘加溢出
在启动时返回错误。App 的实际计费是容量判定的事实源，预算不足明确拒绝。

公开 MQTT API、报文与持久化格式保持原有契约。主要兼容性风险是回调线程改变、
`on_open` 时机改变及新 App 配额下的资源拒绝；调用方不能依赖原网络线程身份。
TurboDB 的 `mysql` 配置入口在适配层映射到当前插件标识 `drivers/mysql`，
不修改外部配置格式。Windows 构建将 SQLite、PostgreSQL 与 CNet 驱动依赖放到
应用目录，以满足插件加载器的受信任 DLL 搜索规则。

## 依赖与迁移

`SaltsBase`、`CMeta`、`SaltsUtils`、`SaltsCNet` 使用分离安装包。
`CHttp::JWT` 是由 Chttp 导出的静态库，公开 cjwt C/C++ 头文件，
其私有 provider 对象合入同一归档；Flowie 不安装第二套 provider。
CRoaring 和 Monocypher 仍为必要 vendor，保留其上游许可。

迁移时 Chttp 0.1.1 提供了所需的 `CHttp::JWT` 与
`chttp_invocation_select_websocket_subprotocol`。
当前版本由 [`.env.example`](../.env.example) 选择。恢复脚本校验并安装发布归档，
设置 `CHTTP_ROOT`。Flowie 的安装配置记录实际链接的 Chttp 精确版本。
TurboDB SDK 也必须与所选编译器匹配。

回滚需同时恢复迁移前的 Flowie 源码及对应 SDK 集合并重新构建；
不能将依赖私有引擎的旧 Flowie 与当前 Chttp 混用。不涉及业务数据迁移。

## 验证范围

最小验证覆盖控制面 TLS 实际响应和响应头、复制/分配失败后的服务复用、
MQTT WS/WSS 消息与关闭、JWT/JWKS 认证。Chttp 增加 H1/H2 子协议协商、
未提供协议、保留头限制、非法 token 和 sealed draft 测试。
迁移时的本地回归使用 Windows LLVM-MinGW。当前验证使用各模块正式 CTest
与正常安装入口，平台范围见 [SDK 资格与产品发布要求](RELEASE_GATE.md)。

按根 README 恢复环境并选择兼容 SDK 后，可重复运行：

```powershell
cmake --preset native-release-user -DBUILD_TESTS=ON -DFLOWIE_BUILD_TESTS=ON
cmake --build --preset native-release-user
ctest --preset native-release-user --no-tests=error --output-on-failure -j 1
python -B -m unittest discover -s cmake/ci -p 'test_*.py'
```

## 2026-11-16 迁移与部署记录

迁移时一次并行全量测试出现 MED 问题，
`test_flowie_control_management_session.c` 的 `tt_make_temp_file` 返回 NULL；
随后该测试独立连续 12 次通过。事实：SaltsBase TinyTest 的 Windows 实现通过
`GetTempFileNameA` 预留临时文件后，再以 `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)`
改为调用方后缀；各 Flowie 测试大量使用同一个 `flo` 三字符前缀。
推论：改后缀释放原临时名称可能导致并发重用，仍需在 SaltsBase 中独立复现和修复。
本次未通过重试、关闭测试或修改生产行为来掩盖该失败。

当前部署入口见[发布 SDK 镜像与 Compose](../deploy/server/README.md#发布-sdk-与-compose)。

2026-11-16 EU 首次部署验收发现 HIGH 阻塞（已按下述方案修复，事实）：发布 SDK 镜像的 Control 可通过
常规 TLS 1.2/1.3、管理登录、Auth 和 ACL 检查，但 MQTT CONNECT 后没有 CONNACK。
隔离数据库与端口复现表明 Auth/ACL 均成功，随后
`flowie_protocol_repository_session_save()` 的 PostgreSQL 事务开始失败；诊断探针捕获
数据库桥接的 `cnet_send_buffer()` 返回 `SALTS_EPERM`，TurboDB 对外报告
`PostgreSQL result stream ended without query completion`。Repository 在启动配置线程
创建连接，运行时却从 Broker owner 线程使用，违反 CNet 连接的线程归属。
需要先明确 Repository 连接与 visit 回调的固定执行线程，再重新验收；不得通过关闭
数据库 TLS、换用其他存储或依赖 TCP healthcheck 绕过。

本次已回滚旧镜像。旧镜像通过 19 项恢复检查，包括 MQTT 3.1.1/5 TCP、MQTT 5 公网 TLS、
QoS 1 PUBACK、订阅允许/拒绝、管理登录/退出、Auth/ACL allow/deny 和七个相邻容器不变。
旧 Control 的直接 TLS 1.3 测试须关闭客户端 middlebox compatibility；常规客户端握手
失败，不能把该恢复结果当作新版发布通过。新版 RSA 只读证书加载已通过 SDK 内存入口修复，
相关 Windows 四项回归通过。

线程归属修复采用每 Repository 一个 Salts 有界单线程池：在存储线程完成连接创建、
schema 校验、事务、读取、visit 回调和连接销毁。调用方继续同步等待完成，输入借用到
返回为止；数据库仍是唯一事实源，CAS、提交失败和回滚语义不变。队列只有一个槽，
沿用调用方串行化前提；同仓库回调重入返回 `SALTS_EBUSY`，回调内关闭仓库违反生命周期契约。

未选择每次操作重建连接，因为会在消息路径反复建立 PostgreSQL TLS 会话；也不迁移
已创建 CNet owner 的线程。所选方案增加每仓库一个线程和一次同步派发，无新增依赖、
schema、端口或配置格式变化。迁移要求 visit 回调不依赖调用线程的 TLS/线程局部状态；
检查范围包含 Broker 启动恢复、跨调用线程写入、回调线程、重入拒绝和正常关闭。
发布前使用独立 PostgreSQL 数据库运行 `test_flowie_protocol_repository_turbodb_live`，
并完成 MQTT CONNECT、QoS 1、鉴权和容器重启后的恢复验收。失败时仅恢复旧镜像选择，
无需数据库格式回退。

同次重启验收还复现了存量会话恢复缺少 `initialized` 标记：数据库行和到期时间保持不变，
重新 CONNECT 却返回 Session Present=0。恢复完成时显式设置该标记，保持原订阅和
会话代次；新增用例先复现失败，再验证恢复后的 CONNACK 与订阅快照。该修复不迁移数据。

最终镜像 `flowie-server:chttp-0.1.1-20261116t055005z-584668800c0d` 已在 EU 部署通过。
事实：Windows 52 项启用测试通过；同一 PostgreSQL direct TLS 跨线程用例在旧库失败、
修复库通过；隔离实例通过 18 项协议检查和容器重启后的持久会话恢复；线上 20 项检查
通过，包括 MQTT 3.1.1/5 TCP、MQTT 5 公网 TLS、QoS 1/2、Control TLS 1.2/1.3、
管理会话与 Auth/ACL allow/deny。另验证公网反向代理登录、Dashboard 和退出成功。
最终 healthy、自动重启 0、运行错误 0，七个相邻容器未重建，Broker V2 / Control V7 保持不变。

真实 PostgreSQL 回归沿用 `FLOWIE_TURBODB_LIVE_TESTS=ON` 和
`FLOWIE_TURBODB_TEST_CONNINFO`。direct TLS 场景显式设置
`FLOWIE_TURBODB_TEST_TRANSPORT=direct_tls`，并提供 `FLOWIE_TURBODB_TEST_REMOTE_HOST`、
`FLOWIE_TURBODB_TEST_REMOTE_PORT`、`FLOWIE_TURBODB_TEST_SERVER_NAME`、
`FLOWIE_TURBODB_TEST_CA_FILE`；仅连接可丢弃的隔离测试数据库。构建并运行
`test_flowie_protocol_repository_turbodb_live` 可复验跨线程写入和固定回调线程。
