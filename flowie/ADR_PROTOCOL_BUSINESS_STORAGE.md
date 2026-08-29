# ADR: 协议数据、集群数据与业务数据边界

## 状态

已接受，2026-08-23 修订。

## 决策

Flowie 明确区分三个事实源：

```text
standalone MQTT protocol facts -> flowie_protocol_repository -> Orm::C
cluster replicated data + log  -> TurboRaft log + state-machine snapshot
Graph/business facts           -> product-owned sink/repository
```

standalone repository 保存 session、subscription、inflight、delivery、Will、principal 与 retained
publication，并用 serializable transaction 和 revision CAS 更新。它不保存 cluster route、owner、
binding、Raft log 或 snapshot。

cluster 不打开 ORM、PostgreSQL、Redis 或 FlowStore 存储。owner、routing 和 publish mutation 先作为
Raft proposal 排序，提交后由 state machine 应用；内存目录仅是 committed log/snapshot 的派生视图。
TurboRaft 的 WalStorage 保存本地日志、term/vote 与 snapshot，DataStream/FlowMQ 负责有界数据和 peer
传输。恢复固定为 restore snapshot 后按序 replay committed log，不存在跨 backend reconciliation。

Graph 业务数据不参与 MQTT Session Present、ACK、takeover、Will、subscription routing 或 cluster
fencing。即使物理引擎相同，也不能共享连接、namespace、migration 或错误语义。

## 不变量与错误语义

- standalone 和 cluster persistence binding 互斥；配置冲突立即返回 `TURBO_EINVAL`。
- cluster durable 只表示 Raft 数据/日志已满足提交契约，不能以本地 queue 接纳代替。
- ORM open/schema/CAS/capacity 失败原样向上返回，不回退到旧 record store。
- snapshot 或 log 损坏导致 cluster 启动失败，不回退 standalone。
- 所有可增长队列、payload、session 与 snapshot 均受配置上限约束。

## 验证

- standalone：ORM schema V2、session/retained round trip、CAS conflict、rollback 与容量。
- cluster：SQLite log recovery、snapshot restore、owner projection、state-machine apply、FlowMQ peer
  transport、DataStream quorum transfer 与 generation lifecycle。
- composition：cluster 配置必须提供 endpoint command port，且不能同时注入 standalone repository。
