# ADR：分层 ACL 主体与 Domain 隔离的通用 MQTT Topic

## 状态

已实施。

本文记录 Flowie Control ACL 当前的控制面语义；用户可见语法以
[ACL_GRAMMAR.md](ACL_GRAMMAR.md) 为准。实现复用了既有 typed runtime rule 和 draft/publish 流程，
但控制面存储和 Management RPC 已切换为 typed subject rule。数据库只接受全新的 v5 schema；旧规则、
旧 schema 和旧 RPC 不迁移、不读取、不注册。published bundle C ABI 和 `/v4/acl/check` wire contract
保持不变，因为它们是 Broker 运行时边界，不是旧控制面规则接口。

## 背景

实施前，Control ACL 文档只能使用 `user <principal-id>` 作为主体，并要求每个 Topic 遵循：

```text
<domain>/groups/<group-path>/devices/<device>/<leaf>
```

这把两个不同维度耦合在了一起：

- 身份维度：用户属于哪些 Role 和 Group；
- 资源维度：MQTT Topic 如何组织。

结果是相同策略必须为每个用户重复保存，而且业务 Topic 必须包含 `groups` 和 `devices`。资源路径中的
Group 只是 Topic segment，却同时被 Repository 当成 Control Group 引用校验，限制了第三方系统使用
自己的 Topic taxonomy。

Core 已经具备本设计所需的 typed subject：`flowie_security_rule_t` 支持 `PRINCIPAL`、`ROLE` 和
`GROUP`，`flowie_security_principal_t` 也携带有界的 effective Role/Group snapshot。当前 Control
compiler 只是把所有文档固定编译为 `PRINCIPAL`。因此应扩展控制面格式并复用现有执行模型，不新增
第二套授权引擎。

## 目标

- 一份 Role 或 Group ACL 文档可服务 Domain 内所有匹配用户。
- 用户 ACL 只在需要个体补充或收紧时存在。
- Topic 仅强制当前 Domain 前缀，不绑定 `groups/devices` 业务结构。
- 保持默认拒绝、显式 deny 全局优先、跨 Domain fail closed。
- 保持 published rule ABI 和 `/v4/acl/check` 运行时契约稳定。
- 保持 draft 校验、原子发布、不可变 `policy_version` 和 Repository 单一事实源。

## 非目标

- 不开放 Domain-wide `any` 主体。Core 的 `ANY` 继续作为底层能力，不进入本版 Control 文法。
- 不引入按 ordinal、文件顺序或“最后匹配”覆盖的 ACL。
- 不增加用户可执行脚本、表达式语言、正则表达式或自定义 matcher 插件。
- 不改变 MQTT Auth、service credential、Management session 或 HTTPS endpoint 的信任边界。
- 不提供跨 Domain Role、Group、Topic 或隐式 fallback。

## 候选方案

### 方案一：继续复制每用户文档

保持当前格式，由管理端为每个用户生成相同 ACL。

- 优点：实现不变。
- 缺点：规则数量随用户数增长，更新容易遗漏，Role/Group 只管理身份而不能成为授权主体。

### 方案二：按主体层级顺序覆盖

依次计算 Role、Group、User，后层规则覆盖前层规则。

- 优点：看起来接近配置继承。
- 缺点：同一用户拥有多个 Role/Group 时没有稳定的天然顺序；ordinal 会意外变成安全语义；用户 allow
  可能静默覆盖上层 deny。

### 方案三：Role/Group 必须先 allow，User 只能收紧

先计算 Role/Group；没有上层 allow 时直接拒绝，User 只能作为第二道 gate。

- 优点：严格保证用户不能取得上层未授予的权限。
- 缺点：现有仅依赖 user allow 的策略会全部失效；User allow 的缺省匹配语义难以解释，也无法表达经过
  审计的单用户补充授权。

### 方案四：适用主体集合加 deny veto

Role、effective Group 和 User 都是同一个 typed rule 模型中的适用主体。所有匹配规则统一聚合：

```text
存在匹配 deny  -> DENY
否则存在 allow -> ALLOW
否则            -> DEFAULT_DENY
```

- 优点：顺序无关、可复验、兼容 Core 现有模型和既有 user allow；用户 deny 可收紧共享权限。
- 缺点：用户 allow 可以补充 Role/Group 未授予的权限，但不能越过任何共享层 deny。

选择方案四。它以确定的 effect lattice 表达继承，不把策略顺序变成隐藏状态。`user` 是新 typed
subject 模型中的一种主体，而不是旧 user-rule 存储接口的兼容入口。

## 主体模型

文档主体扩展为：

| 文法 | 内部 subject kind | 适用条件 |
| --- | --- | --- |
| `role <role-id>` | `FLOWIE_SECURITY_SUBJECT_ROLE` | Role 位于 principal 的 effective Role snapshot |
| `group <group-id>` | `FLOWIE_SECURITY_SUBJECT_GROUP` | Group 位于 principal 的 effective Group closure |
| `user <principal-id>` | `FLOWIE_SECURITY_SUBJECT_PRINCIPAL` | principal ID 精确相同 |

Group closure 包含直接 Group 及其所有 enabled 祖先 Group。因此加入子 Group 的用户同时适用子 Group
和祖先 Group ACL。Role 不建立继承树，多个 Role 以无序集合参与判定。

同一 Domain 最多保存一份相同 `(subject_kind, subject_id)` 的文档。不同 kind 可以使用相同 ID，例如
`role operators` 与 `group operators` 是两个不同主体。

保存和发布时必须验证主体位于请求 Domain 且存在、enabled：

- `user` 引用 enabled principal；
- `role` 引用 enabled Role；
- `group` 引用 enabled Group。

被 draft 或 published policy 作为主体引用的 Role/Group/User 不得直接删除或 disable。调用方必须先删除
引用、成功发布新 policy，再执行主体状态迁移。整个流程继续使用 Management command、revision、
request ID 和 audit，不直接修改数据库。

## 决策语义

一次请求只在 `request.domain == principal.domain == policy.domain` 时继续，否则返回 Domain mismatch。
在相同 policy version 内，适用规则集合为：

```text
applicable = matching_roles
           U matching_effective_groups
           U matching_principal
```

规则还必须同时匹配 action、resource type 和 Topic。最终决策为：

| 匹配集合 | 结果 |
| --- | --- |
| 任意层级存在 deny | deny rule |
| 没有 deny，至少一个层级存在 allow | allow rule |
| 没有匹配规则 | default deny |

该语义具有以下结果：

- Role/Group allow 提供共享权限；
- User allow 可以补充个体权限；
- User deny 可以收紧 Role/Group allow；
- User allow 不能覆盖 Role/Group deny；
- 多个 Role/Group 之间不按名称、创建时间、层级深度或 ordinal 排序；
- `ordinal` 只用于稳定管理、列表与诊断，不影响 allow/deny。

CONNECT 使用文档顶层 effect，SUBSCRIBE/PUBLISH 使用 topic entry effect，二者分别聚合。一个主体的
CONNECT allow 不会隐式授予任何 Topic 权限。

## 控制面文法

### 示例

```text
role publisher allow {
  write topic warehouse/telemetry/%u/+
  write topic warehouse/events/{created,updated}
}
```

```text
group operators allow {
  read topic warehouse/commands/#
}
```

```text
user device-7 allow {
  deny write topic warehouse/telemetry/%u/private
}
```

### EBNF

```text
document       = subject-kind SP subject SP connection [ SP "{" LF entries LF "}" ] ;
subject-kind   = "role" | "group" | "user" ;
connection     = "allow" | "deny" ;
entries        = entry *(LF entry) ;
entry          = [entry-effect SP] access SP "topic" SP topic-pattern ;
entry-effect   = "allow" | "deny" ;
access         = "read" | "write" | "readwrite" ;
topic-pattern  = domain "/" topic-tail ;
topic-tail     = "#"
               | alternatives
               | topic-level *("/" topic-level) [ "/" ("#" | alternatives) ] ;
topic-level    = topic-static | "+" | "%u" | "%c" ;
alternatives   = "{" topic-static "," topic-static *("," topic-static) "}" ;
subject        = identifier ;
domain         = identifier ;
identifier     = 1*(ALPHA | DIGIT | "_" | "." | ":" | "@" | "~" | "-") ;
topic-static   = 1*(ALPHA | DIGIT | "_" | "." | ":" | "@" | "~" | "-" | "$") ;
```

本版把“通用 Topic”定义为不要求固定业务 segment 的有界 MQTT filter，而不是接受任意未经约束的
UTF-8 文本：

- 第一 segment 必须是请求 Domain 的精确 ID，不能使用 wildcard 或 placeholder；
- Domain 后至少存在一个 segment；
- `+`、`%u`、`%c` 必须占据完整 segment；
- `#` 必须占据最后一个完整 segment；
- `%u` 和 `%c` 可出现在 Domain 后任意 segment；运行时替换值必须非空，且不能包含 `/`、`+`、`#`；
- alternatives 保持为最后一个 segment 的有界语法糖，每个候选编译为一条内部规则；
- policy pattern 不允许空 segment、NUL、非法 UTF-8、共享订阅前缀或越过现有长度/展开容量；共享订阅
  request 由 MQTT 协议层解析后，以 inner Topic Filter 执行相同 containment 判定；
- read 规则继续使用 MQTT Topic Filter containment 语义授权 SUBSCRIBE，不能因 filter 仅有交集而放行；
- write 规则使用 MQTT Topic Filter match 语义授权实际 PUBLISH Topic Name。

`groups` 和 `devices` 在 Topic 中只是普通静态 segment，不具有结构关键字或数据库引用语义。新系统
不会读取或迁移旧规则；若新建 typed rule 使用相同的 Topic 文本，则只按本节通用 Topic 文法解释。

### Canonical 格式

- 第一行固定为 `<subject-kind> <subject> <connection>`，block 仍使用两个空格缩进；
- `role`、`group`、`user` 是 document 层关键字；进入 Topic 后按普通静态 segment 处理；
- allow entry 不输出 `allow` 前缀，deny entry 必须输出 `deny`；
- 顶层 connection 为 `deny` 时不能包含 topic block；
- 不接受空行、行尾空格、CRLF 或末尾额外换行；
- formatter 输出必须与提交文本 byte-for-byte 相同；
- `user`、`role`、`group` 都通过同一个 typed subject document formatter 生成，不存在旧 user 文档入口。

## 状态归属与发布

Control Repository 继续是唯一事实源：

```text
User/Role/Group membership ──> Auth principal snapshot
ACL draft ── validate/publish ──> immutable Domain policy version
                                      |
Broker request + principal snapshot ──> /v4/acl/check decision
```

- draft 按 `(domain_id, subject_kind, subject_id)` 保存 canonical document，`ordinal` 只负责域内稳定排序；
- subject kind 是文档中的类型化事实，compiler 写入已有 `flowie_security_rule_t.subject_kind`；
- publish 在同一 Repository 事务内重新验证所有主体、Topic、容量和重复键，然后原子产生新
  `policy_version`；
- 发布失败不改变当前 active policy；
- `/v4/acl/check` 继续加载请求指定的精确 published version，不读取 draft；
- SQLite/PostgreSQL 继续通过同一 Repository port 提供相同语义；
- 资源路径中的普通 segment 不再引用 Control Group。只有 `group <group-id>` subject 和 membership
  才引用 Group 事实。

v5 draft 表保存 canonical text 以及在写入边界验证过的 subject 索引。published rule 包含编译后的
subject kind；索引只能从 canonical document 推导，必须可重建且不能成为第二事实源。

## 身份变更与生效边界

Role assignment 和 Group membership 属于身份事实，不属于 ACL policy。它们改变时不隐式发布新
policy version：

- 新连接或成功 re-auth 获得新的 effective Role/Group snapshot；
- 已连接 session 在 principal `expires_at` 前仍可能携带旧 snapshot；
- 到期时 Broker 必须按现有契约 fail closed，重新认证后才可继续；
- Role/Group 撤权的最坏传播时间因此等于配置的 principal TTL，而不是 ACL publish 延迟。

该边界必须进入运维文档和撤权测试。需要即时撤权的部署应缩短 TTL 或主动断开目标 session；本设计不
增加隐藏 push channel，也不让 ACL endpoint 在不同路径维护第二份 session 状态。

## 接口影响

### 不兼容变更

- 删除 `control.policy.rule.put/list/delete`，改为结构化的
  `control.policy.subject_rule.put/get/list/delete`；
- draft 主键从 `(domain_id, ordinal)` 改为 `(domain_id, subject_kind, subject_id)`；
- 只接受全新的 v5 schema；旧 schema 不迁移、不读取，启动时直接报 schema 不兼容；
- `ordinal` 仅保留为 Domain 内唯一的稳定排序字段；revision、request ID、audit 和原子发布流程保留；

详见 [ADR_TYPED_SUBJECT_RULE_STORAGE_RPC.md](ADR_TYPED_SUBJECT_RULE_STORAGE_RPC.md)。

### 保持不变

- `flowie_security_rule_t`、policy bundle v3 和 SecurityRealm public C ABI；
- `/v4/acl/check` request/response 字段和 fail-closed 状态码；
- MQTT CONNECT、SUBSCRIBE、PUBLISH 的协议级拒绝行为。

### 扩展

- Control parser/formatter 的 subject kind；
- internal ACL document 增加 typed `subject_kind`；
- compiler 不再固定写入 `PRINCIPAL`；
- Repository 验证按 subject kind 查询 User/Role/Group，并以 `(kind, id)` 判重；
- Dashboard 增加 Subject type，再从当前 Domain 的相应实体列表选择 subject；
- Topic builder 不再要求或验证 Control Group path，只验证 Domain-bound MQTT filter。

不引入新的工厂、策略注册表或插件接口。现有 typed rule、parser、validator、compiler 和 Repository
command 边界已经足够，增加通用抽象只会产生无益间接层。

## 容量与性能

继续沿用现有硬上限：每文档 entry、alternatives、单规则长度、compiled pattern、principal Role/Group
数量和每 Domain published rule 总数均有界。Topic segment 数量由 511-byte pattern 上限约束，校验采用
线性扫描，不使用递归；alternatives 只允许单个终端集合，因此不存在展开乘积。

预期影响：

- Role/Group 共享规则可显著减少重复 user rule 和 published rule 数量；
- 一个请求会适用多个 Role、Group 与 User subject，候选集合可能比单 user 模型更大；
- 当前 evaluator 的总扫描成本仍受 `FLOWIE_SECURITY_MAX_RULES`、`MAX_ROLES`、`MAX_GROUPS` 限制；
- 不因本设计提前引入缓存或新索引。只有 benchmark/profile 证明授权匹配占总耗时至少 20% 后，才优化
  subject candidate index；优化不得改变 deny veto 和 matched-rule 诊断语义。

## 错误语义

- 未知 subject kind、非法 Topic、跨 Domain pattern、主体不存在/disabled、重复 `(kind,id)`、展开或
  容量溢出立即失败；
- draft 中任意文档失败会使整个 validate/publish 失败，不发布部分策略；
- policy version 不匹配、principal 过期、provider/Repository 故障继续 fail closed；
- 不把无 Role/Group、无规则或无匹配解释为匿名权限；
- 不自动把无效新格式降级为旧 parser，也不在失败后尝试 Mosquitto ACL 格式。

## 部署与回滚边界

### 全新 v5 数据边界

- v5 是不兼容 schema，不提供旧规则转换、导入、读取、双写或启动时自动升级。
- 部署前如需保留旧环境，应在部署系统层面对旧数据库做独立备份；新版本不会消费该备份。
- 新环境必须创建空的 v5 Repository，再通过 `control.policy.subject_rule.*` 提交 typed subject rules 并发布。
- 所有 Control 写节点必须同时切换；不得让旧、新 binary 写同一个 Repository。

### 回滚

- 应用 binary 可回滚，但 v5 Repository 不能交给旧 binary 继续使用。
- 回滚必须切换到部署前的完整旧环境快照，或创建新的空 Repository；不允许把 v5 数据反向转换成旧规则。
- published policy version 不降级，不直接修改数据库，不以旧 RPC 或兼容 parser 作为恢复路径。

## 风险

- **HIGH**：若采用顺序覆盖，用户 allow 可能绕过上层 deny。实现和测试必须固定 deny veto，ordinal
  不得参与决策。
- **HIGH**：Role/Group 撤权对已有 MQTT session 受 principal TTL 约束；部署必须接受并验证该传播上界。
- **HIGH**：SUBSCRIBE 必须验证 requested filter 被 allow filter 完整包含，不能使用简单字符串匹配或
  overlap 判断。
- **MED**：移除 Topic Group path 引用后，Group 删除约束只来自 typed group subject 和 membership；
  旧的资源 segment 不再保护 Group 实体。
- **MED**：共享规则减少存储量，但增加单 principal 适用 subject 数；需用 PicImpact 规模数据验证延迟和
  容量。
- **LOW**：Role、Group、User 可有同名 ID，Dashboard 和审计必须同时显示 subject kind，避免误操作。

## 验证范围

实施必须至少覆盖：

1. parser/formatter：三个 subject kind、generic Topic、placeholder、wildcard、alternatives 和 canonical
   round trip；
2. validation：主体存在/enabled、typed duplicate、跨 Domain、非法 wildcard、容量与 publish rollback；
3. compiler：三个 subject kind 正确写入 internal rules，ordinal 不参与授权决策；
4. decision matrix：Role allow + User deny、Role deny + User allow、Group ancestor allow、多个 Role 冲突、
   无匹配 default deny；
5. MQTT matcher：PUBLISH match、SUBSCRIBE containment、`+/#`、`%u/%c`、共享订阅 inner filter 和
   `$SYS`/Domain namespace 边界；
6. identity lifecycle：Group ancestor closure、Role/Group assignment 变更、principal TTL/re-auth；
7. Repository：SQLite 与 PostgreSQL 的 draft、reference、publish、exact version 和并发事务语义一致；
8. Management/Dashboard：typed subject CRUD、分页、审计、权限矩阵和 canonical preview；
9. HTTPS/endpoint：ACL v4 wire contract 不变，CONNECT/read/write allow/deny 与故障 fail closed；
10. incompatibility：旧 schema 启动失败、旧 RPC method-not-found、v5 不被旧 binary 复用。

验证顺序为 parser/compiler 最小测试、Repository contract、ACL decision、真实 MQTT/TLS integration，最后
执行完整 Control/Flowie 回归和有界性能测试。
