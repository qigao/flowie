# Flowie 配置式 Broker 概念与术语

## 1. 定义范围

本文规定 Flowie 服务端配置与应用集成的术语。独立 `flowie_server` 支持 CLI 和 YAML 两种入口。
独立 IoT 网关使用 **Domain Configuration** 配置部署资源，使用 **Domain Packages** 激活
签名 ACL 与常驻 RulesForge 流水线。 [IoT 网关部署](../deploy/server/README.md#iot-网关部署)

使用 TurboFlow 的宿主还可从 provider registry 选择能力、用 YAML 创建具名实例，再用 `.flow` Graph
连接它们。

只有这一完整的配置式 broker 才是本文所说的“基于 TurboFlow 的典型消息处理应用”：

```text
Flowie MQTT flowie/protocol/session owner
  + product provider registry   # 宿主编译并注入哪些能力
  + YAML resolved config        # 创建哪些 endpoint/channel/adapter 实例
  + TurboFlow Graph             # source/stage/sink 的拓扑和处理顺序
  = configured Flowie broker
```

Flowie 的使用方式如下：

| 使用方式 | Flowie 提供什么 | 是否是配置式 TurboFlow 应用 |
|---|---|---|
| 协议库 | MQTT parser、packet view、编码与校验 | 否；没有运行时或 Graph |
| 嵌入式 endpoint Core | 调用方直接创建/持有的 MQTT owner 与同步 dispatch callback | 否；不创建 Graph |
| Endpoint + Graph adapter | 同一 Core 的可选 TurboFlow source/sink 边界 | 不一定；取决于宿主是否完成产品拓扑 |
| 独立 `flowie_server` | 从 CLI 或 YAML 装配 endpoint、协议 repository 与可选 HTTPS Auth/ACL | 否 |

Endpoint Core 可以经薄 adapter 成为另一个 TurboFlow 应用的一部分，但 Core 本身不代表已经存在
data source、data sink、持久化和业务处理拓扑。

### 可交付产品组合

| 产品模式 | MQTT session / retained | Cluster 业务数据 | 日志与快照 | MQTT ACK 边界 |
|---|---|---|---|---|
| standalone | `Orm::C` repository，原生默认使用文件 SQLite | 不适用 | 不适用 | Endpoint 选择的 settlement 边界 |
| 实验 server/cluster | TurboRaft committed state machine | TurboRaft committed owner/PUBLISH 数据 | `TurboRaft::WalStorage` | quorum commit 后的 owner settlement |

standalone 用于开发和单机运行，repository 是该进程代际的唯一协议事实源。server/cluster 不创建独立
ORM、Redis、PostgreSQL、FlowStore、route store 或 outbox；业务命令和 Raft 日志使用同一
`TurboRaft::Service` 提交序列，state machine 只能应用已提交 entry。

部署参数、协议数据库和凭据注入见 [服务端指南](SERVER_GUIDE.md)与[容器部署](../deploy/server/README.md)。

## 2. 规范术语

### Provider

`provider` 是 composition root 视角的实现提供者。它把一种受信任的原生能力注册到产品宿主，
例如 `turbo_flow_product_adapter_provider_t`、TurboFlow Policy resource provider、auth provider
factory 或 ACL policy provider factory。Provider 决定“这个宿主能够创建什么”，不决定消息
在 Graph 中从哪里流向哪里。

文档不得单独使用含义不明的 `data provider`。必须根据实际接口写成以下术语之一：

- **adapter provider**：根据 YAML `adapters.<name>.kind` 创建 Graph adapter；
- **resource provider**：根据 channel 创建 TurboFlow Policy 等 Graph resource；
- **auth provider**：验证 credential，返回 principal 或拒绝；bundled 产品只注册 `https`；
- **policy provider**：提供版本化 ACL bundle；
- **facts provider**：把 `turbo_flow_msg_t` 投影为 TurboFlow Policy facts；
- **external data service**：Redis、PostgreSQL、HTTP 服务等进程外系统。

这些 provider 接口用途不同，不能因为名称中都有 `provider` 就互换。

### Backend

`backend` 是某个 provider 内部选择的具体实现，例如 standalone TurboDB ORM repository 的
`sqlite`，或 auth provider 的 `https`。Backend 是实现细节，不是 Graph 角色，也不自动成为 data
source 或 data sink。server/cluster persistence 不通过 backend 字符串选择：它固定由 TurboRaft service 与
WalStorage 共同拥有。auth provider 也不复用 ORM repository；身份数据库只能留在 HTTPS 认证
服务内部。

### Adapter

`adapter` 是 Graph 可引用的具名边界。它把外部协议或存储格式转换为
`turbo_flow_msg_t`，或把 `turbo_flow_msg_t` 交给外部系统。YAML 创建 adapter 实例，`.flow`
决定该实例在拓扑中的位置。

Adapter 应保持为薄边界：负责格式转换、生命周期和错误映射，不拥有 MQTT session 规则或
业务路由策略。

### Data source、data sink 与 transform

这三个词是 Graph 视角的角色，不是 provider 类型：

| Graph 角色 | 定义 | 所有权结果 |
|---|---|---|
| **data source** | 把外部事件转换为 `turbo_flow_msg_t` 并发布到 Graph | 发布成功后按 source 契约转移或保留消息所有权 |
| **data sink** | 消费 `turbo_flow_msg_t` 并产生外部副作用 | 成功只证明该 sink 的契约完成 |
| **transform** | 消费消息并产生修改后的消息或投影 | 输出继续由 Graph 管理 |

同一个 adapter 实现可以支持多个角色。例如 HTTP server 可以接收 request 并发送 reply，
Flowie endpoint 可以发布 admitted PUBLISH 并接收 MQTT fan-out packet。实际允许的角色由
adapter schema 与 Graph 节点共同约束，不能从 `kind` 名称猜测。

Data sink 也不等于 durable sink。只有 adapter 契约明确规定事务已提交，并且 settlement 绑定
到该提交边界时，成功结果才能称为 `durable`。

### Channel 与 resource

`channel` 是 YAML 中具名、带 owner 和容量/连接配置的共享资源。Graph operation 可以通过
`resource <channel-name>` 引用 TurboFlow Policy RuleSet 等资源。Channel 不是消息，也不是
Graph edge。

### `turbo_flow_msg_t`

`turbo_flow_msg_t` 是 Graph 的统一消息 envelope。Provider 内部的 MQTT frame、
HTTP request view、Redis entry view 等短生命周期对象不能直接跨异步边界；进入 Graph 前必须
转换为满足所有权契约的 `turbo_flow_msg_t`。

### TurboDB ORM

TurboDB ORM 独立于 Flowie。standalone 的 `flowie_protocol_repository` 直接依赖
`Orm::C`，用类型化 schema、serializable transaction 和 revision CAS 保存 session、
subscription、inflight、delivery、retained 与 Will 事实。独立 Broker 通过
`--protocol-store-driver` 和 `--protocol-store-options` 选择驱动。打开、建表或恢复失败会使启动失败。该 repository
不参与 server/cluster；server/cluster 的业务数据、Raft log 和 snapshot 全部由 TurboRaft 拥有。

### Session owner 与 session store

standalone 的 TurboDB ORM flowie/protocol repository 是 MQTT session、subscription、packet ID、
inflight QoS、Will 和 retained 状态的唯一事实源。Flowie session owner 只持有从 repository
重建的单 owner 工作缓存；`session_store` 只是旧配置别名，不是另一个 store，也不是用户 Graph
中的 data sink。server/cluster 下相同业务事实归 TurboRaft committed state machine 所有，endpoint-local
repository 必须缺席。

普通 PUBLISH 的网络帧和解析缓冲是传输临时态，不会成为 session 事实；server/cluster 中需要复制的业务
数据必须编码为有界 TurboRaft entry，经 quorum commit 后应用，不能由 Flowie 内部容器独立推进。

### TurboFlow Policy

TurboFlow Policy 是可选的 Graph processor/resource。Facts provider 从 `turbo_flow_msg_t` 提取 MQTT topic、
QoS、payload 等受限视图，RuleSet 执行过滤、转换和路由。Policy 不是 data source、data
sink 或 auth provider，也不能推进 MQTT session 状态或生成协议 ACK。

## 3. 同一对象在不同视角下的名称

同一个外部系统会因观察层级不同而有不同叫法。应先说明视角，再使用术语：

| 观察视角 | 名称 | 正确含义 |
|---|---|---|
| standalone 协议持久化 | TurboDB ORM repository | 保存 session/retained 等协议事实，不进入 Graph |
| server/cluster 一致性 | TurboRaft service/state machine | 提交并应用 owner/PUBLISH 业务数据 |
| server/cluster 持久化 | TurboRaft WalStorage | 保存 term/vote、Raft log 与 snapshot |
| Graph source/sink | 外部系统 adapter | 显式消费或生成 `turbo_flow_msg_t`，不恢复协议状态 |
| 认证系统边界 | HTTPS auth service | bundled Flowie 唯一可配置的认证来源 |
| 认证系统内部 | auth service 的用户数据库/目录 | Flowie 不直接访问，也不由 Graph 配置 |

因此“storage”不足以表达状态归属。必须写明它是 standalone ORM repository、TurboRaft
log/snapshot、committed state machine，还是 Graph source/sink adapter。

## 4. 配置式 Broker 的职责分配

### Product provider registry

Composition root 注册当前二进制允许的 adapter/resource/auth/policy provider。未注册的
adapter `kind` 在 product preflight 失败；Graph 引用但没有 provider 的 resource `kind` 在
assembly 失败；auth/policy backend 必须精确匹配一个已注册 factory。Registry 是宿主能力
白名单，不保存业务拓扑。

### YAML

YAML 定义部署实例和边界条件：endpoint、flowie/transport、channel、backend、容量、超时、secret
reference、RuleSet 和 adapter 配置。YAML 不定义 Graph edge，也不隐式选择业务输出。

### `.flow` Graph

Graph 定义 source、operation、transform、sink、分支以及 save/store/after-process 的顺序。
Data source 和 data sink 只有被 Graph 显式引用后才属于当前 broker 数据路径。

### Flowie flowie/protocol/session owner

Protocol owner 在 Graph 之前完成 framing、协议校验、认证、ACL 和 inflight admission；在 Graph
或 settlement 边界完成后决定 MQTT ACK。CONNECT、AUTH、SUBSCRIBE 等控制事务不因为存在
Graph 就自动成为业务消息。

## 5. 标准数据路径

```text
network bytes
  -> Flowie endpoint parser
  -> auth provider / ACL policy provider
  -> session owner admission
  -> turbo_flow_msg_t
  -> optional TurboFlow Policy / transforms
  -> explicit business data sink and/or MQTT fan-out
  -> settlement result
  -> session owner emits flowie/protocol ACK
```

必须保持以下不变量：

1. Session owner 是 MQTT 状态的唯一写 owner；Graph 不直接修改 session。
2. Auth/policy provider 属于安全控制路径，不是业务 data source。
3. TurboDB ORM 只服务 standalone flowie/protocol repository；server/cluster 业务数据与日志由 TurboRaft 统一拥有。
4. Graph admission、处理完成和 durable commit 是三个不同边界。
5. 未出现在 Graph 中的 adapter 不参与消息处理；Flowie 不猜测或自动添加存储节点。
6. 外部宿主负责解析和编译其 Graph，并装配所选存储 owner。独立 `flowie_server --check`
   校验 Broker 配置与 standalone repository 装配。持续可用和恢复由各模式的事务或 log/snapshot 测试验证。

## 6. 配置参考

- [Broker YAML 测试 fixture](../flowie/server/tests/flowie_server_https_secure.yml)展示 profile、endpoint 和 HTTPS provider 结构。
- [服务端使用指南](SERVER_GUIDE.md)给出当前独立 Broker 的预检、启动和存储配置。
- [协议数据与业务数据边界](ADR_PROTOCOL_BUSINESS_STORAGE.md)记录外部 Graph 宿主的状态所有权与 settlement 约束。
