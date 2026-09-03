# Flowie 长期运行日志策略

本文记录 2026-08-24 EU 原生 Debug 压测对 Flowie、CoroNet 和 tlog 日志的分析，并定义后续改造与验收标准。
该数据是迁移前历史基线；当前主线网络实现为 Salts CNet/CHTTP，不应把本文的 CoroNet 日志数量外推为当前行为。
目标是在不降低故障可诊断性的前提下，让日志能够长期保存、准确表达运行状态，并且不会随正常连接和消息数量
无界增长。

## 1. 实测基线

证据来自 `/root/dev/runs/20260824T093200Z`，本地副本位于
`C:\projects\cpp\artifacts\eu-results\20260824T093200Z`。三级 fan-out 测试全部通过，日志中没有
WARN、ERROR、FATAL、空 component/source 或敏感字段名。

| Tier | 客户端 | 日志行 | 字节 | CoroNet 行 | Flowie 行 | 每客户端字节 |
|---:|---:|---:|---:|---:|---:|---:|
| 16 | 96 | 340 | 62,989 | 308 | 32 | 656.1 |
| 32 | 192 | 682 | 126,594 | 618 | 64 | 659.3 |
| 64 | 384 | 1,320 | 245,256 | 1,192 | 128 | 638.7 |

Tier 64 的 1,320 行由以下事件组成：

| 事件 | 行数 | 含义 |
|---|---:|---|
| `CoroNet.epoll read-event status=-4095 bytes=0` | 424 | 全部是 `TURBO_EOF`，属于正常对端关闭 |
| `CoroNet.epoll shutdown-begin` | 384 | 每个连接一次正常 shutdown |
| `CoroNet.epoll stream-close` | 384 | 每个连接一次正常 close |
| `Flowie.Endpoint qos2-window ack=PUBREC` | 64 | 每个 QoS 2 subscriber 一次满窗采样 |
| `Flowie.Endpoint qos2-window ack=PUBCOMP` | 64 | 每个 QoS 2 subscriber 一次释放采样 |

CoroNet 正常关闭路径占该档 90.3% 的日志行和 86.9% 的字节。424 条 EOF 来自 380 个不同 stream，
其中 44 个 stream 记录了两次终止读事件。删除正常关闭逐条日志后，这一档理论上可从 1,320 行降到
128 行，减少 90.3%；字节从 245,256 降到 32,064，减少 86.9%。这是基于本次日志分类的计算值，
不是生产吞吐承诺。

P0a 实现后的证据位于 `/root/dev/runs/20260824T104051Z-flowie-log-p0`。同样的 16/32/64 测试均通过；
Tier 64 实测为 128 行、32,064 字节，分别比基线减少 90.30% 和 86.93%，与上述计算一致。该档只保留
128 条有上界的 `Flowie.Endpoint qos2-window` DEBUG，正常 MQTT FIN 没有产生 `CoroNet.epoll` 日志；单独
注入 TCP RST 时产生且仅产生一条 `stream-terminal operation=read status=-104 reason=Connection reset
by peer action=close-stream`。

P0b 在同一 run 下增加了 accept/reactor 的行为故障注入。修复前，`RLIMIT_NOFILE` 注入的 `EMFILE` 在
约 163 ms 内产生 431 条 listener 错误；一次 `epoll_wait/EBADF` 只产生一条日志，但缺少结构化字段。修复后
两种故障都恰好记录一条 ERROR，分别使用 `operation=accept action=close-listener` 和
`operation=epoll_wait action=stop-reactor`，并包含负数 status 与可读 reason。随后重跑 16/32/64：Tier 64
仍为 128 行、32,046 字节，307,200 次内容投递通过，WARN/ERROR/FATAL 为 0。实际 Flowie 进程上的正常
FIN 为 0 条 CoroNet 日志，TCP RST 仍恰好产生一条完整记录。

按本次每次断连约 555 字节的 CoroNet DEBUG 量估算，持续 100 次断连/秒约产生 4.8 GB/天，
1,000 次断连/秒约产生 48 GB/天。因此即使所有记录都处于 DEBUG，长期临时开启 DEBUG 仍有明显磁盘风险。

## 2. 准确性问题

### 2.1 正常 EOF 不应表现为故障

基线版本的 `turbo_stream_epoll.c` 对所有非零 read status 记录 `read-event`，正常 EOF 因而只显示为负数
`-4095`。数值本身不能告诉操作者这是正常关闭还是 I/O 故障，也没有写明后续动作。正常 EOF 应只增加
计数器，不逐条写日志；异常 status 才记录稳定的 `operation/status/reason/action` 字段。

### 2.2 一个事实只能有一个日志所有者

基线版本的一次正常断连会经过 `read-event`、`shutdown-begin` 和 `stream-close` 多个记录点。底层 transport、
session 和业务层都可以观察同一个失败，但只能由真正消费错误并决定动作的边界记录它。下层如果必须保留
诊断信息，应使用计数器或受控 trace，而不是再发一条相同严重度的独立日志。

### 2.3 临时地址和 fd 不是连接身份

指针和 fd 会复用，只适合单进程短时间调试，不能作为跨日志、跨重启的唯一关联键。需要关联时应由 Flowie
生成不含客户身份的进程内 connection/session sequence，并同时保留 transport、operation 和生命周期阶段。

### 2.4 shutdown 统计必须说明快照时刻

基线版本的 `flowie_server.c` 在发布 `logging-shutdown` 之前读取 `written/dropped/pending`，之后才 flush。
因此这条记录不包含它自身，`written` 和 `pending` 也不是 flush 后的最终值。应先 flush，再采集并用
`written_before_summary`、`pending_before_summary` 这样的精确字段名发布摘要，最后再次 flush。若要求包含摘要
自身的最终审计值，则需要独立同步 sink，不能用被测异步 logger 自证最终状态。

tlog 当前把 pool 分配失败和 disruptor acquire 失败合并为一个 `dropped`，并且没有 queue high-water mark。
总数适合报警，但不足以定位丢失原因；在增加公共 API 前，先在 tlog 内部分开计数并验证其并发语义。

## 3. 事件与级别规则

| 事件 | 生产默认行为 | DEBUG 行为 | 指标 |
|---|---|---|---|
| 正常 EOF、正常 close、主动 shutdown | 不记录 | 默认仍不逐条记录 | `normal_eof_total`、`normal_close_total` |
| transport 异常 | 由消费错误的边界记录一条 WARN/ERROR | 可附一条同 owner 的上下文 | 按 reason 分类计数 |
| accept/reactor 失败 | ERROR，包含 operation、status、reason、action | 相同 | 失败和恢复计数 |
| 慢 subscriber 隔离 | 保留 power-of-two 采样 WARN | 可增加资源上下文，禁止 payload/身份 | 隔离总数、当前数 |
| QoS 2 window | 不逐连接记录 | 仅用于限定测试或按连接一次采样 | 满窗/释放总数、峰值 |
| tlog drop/backlog | drop 增长时 WARN，恢复时一条 INFO | 提供原因分类 | dropped、queue、queue high-water |
| 启动、配置、停止 | 各阶段一条结构化 INFO | 相同 | uptime、退出原因 |

严重度由“需要什么动作”决定，而不是由代码所在层决定：

- `ERROR`：当前操作失败，服务必须降级、拒绝或退出；包含采取的动作。
- `WARN`：服务继续运行，但资源或正确性风险需要运维关注。
- `INFO`：低频生命周期、有效配置和汇总结果。
- `DEBUG`：临时诊断；也必须满足有 owner、有原因、数量有上界。

日志不得写协议 payload、认证信息、topic/client identity 或数据库连接串。计数日志只在值变化时发送；周期汇总
最长每分钟一条，并在没有增量时静默，避免把 heartbeat 变成新的日志源。

## 4. 数量预算

以下预算应成为测试断言，而不是文档建议：

- 正常连接生命周期：生产 INFO 及以上为 0 行；DEBUG 默认也为 0 行。
- 单次异常：同一故障链最多 1 条 WARN/ERROR，由最终处理边界拥有。
- 正常消息和 ACK：生产日志为 0 行；DEBUG 只能是明确启用、每连接有上界的采样。
- 启动和正常停止：每阶段不超过 10 行，配置只记录实际生效值且不含敏感信息。
- 后台汇总：有增量时最多 1 条/分钟；drop 增长使用首次加 power-of-two 采样，恢复只记一次。
- 每次压测同时检查日志行数、字节数、各模板计数和敏感字段，而不只检查最高级别。

磁盘容量必须按 `每天字节 = 平均记录字节 × 每秒事件数 × 86,400` 计算，并结合 rotation、保留天数和
最坏流量设置告警。运行时级别切换必须有自动恢复期限，避免 DEBUG 被永久遗留。

## 5. 分阶段改造

### P0：不改变公共接口

1. 不再记录正常 `TURBO_EOF` 的 `read-event`。
2. 删除正常路径的 `shutdown-begin`、`stream-close` 和 `connect-immediate` 逐条 DEBUG。
3. accept/reactor/read 异常增加可读 reason 和明确 action，并保证只由一个边界记录。
4. 修正 shutdown 指标的 flush 顺序与字段名。
5. 更新 CoroNet logging test：正常 close 断言零行，注入异常断言恰好一条且字段完整。

P0 已完成。read、accept 和 reactor 异常都有可执行的 Linux 行为测试；accept 使用进程级 fd exhaustion，
reactor 使用仅链接到测试可执行文件的 `epoll_wait` 包装，不向生产库暴露故障注入 API。测试同时断言事件
数量、ERROR 级别、component 以及 operation/status/reason/action，不使用源码字符串搜索代替运行行为。

### P1：进程内统计与低频摘要

增加 normal EOF/close、异常 reason、QoS window、slow subscriber、queue high-water 等内部计数；只在
值发生变化且达到汇总周期时输出。为 drop 增长和 backlog 恢复增加状态转换日志，避免每次失败重复报警。

### P2：公共观测接口

在获得接口变更确认后，为管理端或 metrics sink 暴露 typed snapshot，并将 pool failure、queue acquisition
failure 分开。公共 API 需要定义原子性、复位行为、溢出、关闭阶段以及 ABI 兼容策略。

## 6. 验收方法

1. 重跑 16/32/64 和重复 384-client 内容校验，消息数、multiset、sequence 和资源门槛必须继续 PASS。
2. Tier 64 正常负载的 CoroNet 生命周期逐条日志为 0；总日志行和字节满足 P0 预算。
3. 分别注入 EOF、connection reset、accept failure、reactor failure、tlog pool/queue 压力；正常 EOF 不报警，
   每个异常链恰好一条可读记录，并带 operation/status/reason/action。
4. 验证 logger drop 分类之和等于总 drop，queue high-water 不小于观测 queue，flush 后 pending 为 0。
5. 进行至少 30/60 分钟 soak，同时检查日志生成速率、rotation、磁盘余量和 DEBUG 自动恢复。
6. 对日志执行 payload、client/topic、credential、connection string 和空 component/source 扫描。

P0 已包含日志降噪、read/accept/reactor 异常单点记录、shutdown 快照语义和回归测试。P1 内部统计和
P2 公共接口仍应分别实现和验证，避免把日志降噪、指标语义与 ABI 变化混在一次修改中。
