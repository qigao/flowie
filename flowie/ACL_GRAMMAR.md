# Flowie Control ACL 文法与使用

本文说明 Flowie Control 的主体化 ACL 文档、MQTT 权限语义、受限 topic filter，以及通过
Control UI 或 Management JSON-RPC 发布规则的方法。

该语法由 Flowie 控制面实现，不是第三方集成提供的语法，也不是 Mosquitto ACL 文件格式。Broker
只消费 Control 发布的 typed rule bundle；第三方系统通过 Management API 提交同一份 canonical 文档。

实现事实源为：

- lexer：[control/parser/acl_lexer.re](../control/parser/acl_lexer.re)
- Lemon grammar：[control/parser/acl_grammar.y](../control/parser/acl_grammar.y)
- parser、canonical formatter 和 compiler：[control/flowie_control_acl.c](../control/flowie_control_acl.c)
- Repository 主体与 Domain 校验：[control/flowie_control_store.c](../control/flowie_control_store.c)

## 完整示例

一条 Role 规则可供当前 Domain 内所有有效拥有该 Role 的用户使用：

```text
role publisher allow {
  write topic root-a/telemetry/%u/{event,heartbeat,process}
  read topic root-a/commands/%c/+
  deny readwrite topic root-a/telemetry/%u/private
}
```

也可以为 Group 或单个 User 建立规则：

```text
group operators allow {
  read topic root-a/shared/#
}
```

```text
user device-7 deny
```

## 主体与求值语义

顶层主体控制 MQTT CONNECT，并归属整份文档：

```text
role <role-id> allow|deny
group <group-id> allow|deny
user <principal-id> allow|deny
```

- `role` 主体匹配用户的有效 Role；`group` 主体匹配用户的有效 Group；`user` 主体精确匹配用户。
- 保存和发布时，所引用的主体必须存在于当前 Domain 且 enabled。
- 同一 Domain 内，`(主体类型, 主体 ID)` 最多有一份 draft ACL；相同 ID 可分别用于 Role、Group、User。
- 一个用户先继承适用的 Role/Group 规则，再叠加自身 User 规则。所有适用规则共同求值，不按层级短路。
- 任一适用规则显式 deny 即拒绝；否则任一适用规则 allow 即允许；均未匹配则默认拒绝。
- `ordinal` 只提供稳定存储和输出顺序，不改变 deny 优先语义。
- 顶层 `deny` 不能带 topic block；顶层 `allow` 没有 block 时只允许 CONNECT。

topic block 中每条语句控制 MQTT 操作：

| 关键字 | MQTT 权限 |
| --- | --- |
| `read` | SUBSCRIBE |
| `write` | PUBLISH |
| `readwrite` | SUBSCRIBE 和 PUBLISH |
| 无 effect 前缀 | allow |
| `deny` 前缀 | deny |

## 语法

以下 EBNF 描述 parser 接受的结构。`SP` 表示一个或多个空白字符，`LF` 表示换行：

```text
document       = subject-kind SP subject SP connection [ SP "{" LF entries LF "}" ] ;
subject-kind   = "user" | "role" | "group" ;
connection     = "allow" | "deny" ;
entries        = entry *(LF entry) ;
entry          = [entry-effect SP] access SP "topic" SP topic-pattern ;
entry-effect   = "allow" | "deny" ;
access         = "read" | "write" | "readwrite" ;
topic-pattern  = domain "/" segment *("/" segment) ;
segment        = static | "%u" | "%c" | "+" | terminal-hash | terminal-alternatives ;
terminal-hash  = "#" ;
terminal-alternatives = "{" static "," static *("," static) "}" ;
subject        = identifier ;
domain         = identifier ;
identifier     = 1*(ALPHA | DIGIT | "_" | "." | ":" | "@" | "~" | "-") ;
static         = 1*(ALPHA | DIGIT | "_" | "." | ":" | "@" | "~" | "-" | "$") ;
```

`#` 与 alternatives 只能是最后一个 segment。`%u`、`%c`、`+` 必须各自占据完整 segment；不接受
`device-%u`、`prefix+` 或空 segment。第一段 Domain 只接受 `identifier`，不能使用 wildcard、placeholder
或 `$`。topic 至少包含 Domain 与其后的一个 segment。

`groups`、`devices` 等名称只是普通资源 segment，不再引用 Control Group，也不会隐式改变 ACL 主体。
旧的 `<domain>/groups/.../devices/...` topic 仍是新文法的合法子集。

## Canonical 格式

Repository 要求输入与 formatter 生成的 canonical 文本 byte-for-byte 相同：

- 第一行为 `<subject-kind> <subject> allow {`；每条 topic 语句独占一行并缩进两个空格；最后一行只有
  `}`。
- allow topic 语句不写 `allow` 前缀；`allow read topic ...` 可被 parser 接受，但 Repository 会因
  非 canonical 格式拒绝。
- deny topic 语句必须以 `deny ` 开头。
- 不允许空行、行尾空格、CRLF 或末尾额外换行。
- 无 block 文档只有一行，例如 `role publisher allow` 或 `user device-7 deny`。

Control UI 会从类型化主体表单生成 canonical 文档，并在提交前显示预览。

## Topic filter 与 placeholder

- 静态 segment 支持字母、数字、`_ . : @ ~ - $`；Domain 不支持 `$`。
- `%u` 精确替换为当前 MQTT CONNECT username，`%c` 精确替换为 client ID。运行时值必须非空，且
  不能包含 `/`、`+` 或 `#`。
- `+` 匹配一个 topic level；终端 `#` 匹配剩余 topic levels。
- 终端 `{event,heartbeat,process}` 包含 2 到 16 个互不重复的静态候选，每项编译成一条内部规则。
- 文档主体、MQTT username 与 client ID 是不同字段，不要求彼此相同。
- 文档内每个 topic 的第一段必须与 Management 请求选定的 Domain 完全相同。

## 容量限制

| 项目 | 限制 |
| --- | ---: |
| 一个 Domain 中的 draft ordinal | `0..4095` |
| 同一 `(主体类型, 主体 ID)` 的 ACL 文档 | 1 |
| 一个 ACL 文档的 topic 语句 | 64 |
| 终端 alternatives | 16 |
| 主体或 Domain ID | 255 bytes |
| 编译后的单个 topic pattern | 511 bytes |
| Repository 内部 ACL 文档 | 16383 bytes |
| Control UI topic 输入 | 16000 characters |
| Management JSON-RPC `rule_line` | 2047 bytes |
| 一个已发布 bundle 的内部规则 | 4096 |

每份文档固定产生 1 条 CONNECT 规则；普通 topic 语句产生 1 条规则，alternatives 产生候选值数量的
规则。`control.policy.validate` 返回展开后的 `rule_count` 与 `deny_rule_count`。

## 通过 Control UI 使用

1. 使用具有 `policy_admin` 权限的 Domain 账号登录 Flowie Control。
2. 打开 `ACLs` 并选择 `Add`。
3. 选择 Subject type，再选择该类型下的 enabled User、Role 或 Group。
4. 选择 Connection 行为和稳定的 Evaluation order。
5. 在 Topic permissions 中每行输入一条 topic 语句，不输入外层主体与 `{}`。
6. 添加到 draft；完整检查后选择 `Publish`。

修改或删除 draft 不会立即改变 active policy。只有成功 Publish 后 Broker 才取得新版本；发布失败时旧
版本继续有效。不要直接修改 PostgreSQL 或 SQLite 中的 ACL 表。

## 通过 Management JSON-RPC 使用

Management RPC 的 endpoint、登录与 bearer session 见
[MANAGEMENT_RPC_API.md](MANAGEMENT_RPC_API.md)。`rule_line` 是 JSON string，因此换行写为 `\n`。

```json
{
  "jsonrpc": "2.0",
  "id": "acl-put-1",
  "method": "control.policy.rule.put",
  "params": {
    "domain_id": "root-a",
    "ordinal": 10,
    "rule_line": "role publisher allow {\n  write topic root-a/telemetry/%u/{event,heartbeat}\n  read topic root-a/commands/%c/+\n}",
    "request_id": "acl-publisher-v1"
  }
}
```

随后调用 `control.policy.validate` 校验整个 draft，再调用 `control.policy.publish` 原子发布新版本。同一次
业务写入在传输结果不确定时必须复用原 `request_id`；不同写入不能复用。

## 常见拒绝原因

- 主体类型不是 `user`、`role` 或 `group`。
- 主体不存在、已 disabled，或同一 Domain 的其他 ordinal 已使用相同 `(类型, ID)`。
- 文本可解析但不是 canonical 格式，或提交了旧 pipe 格式/Mosquitto ACL 行。
- topic 的 Domain 与请求 Domain 不一致。
- topic 包含空 segment、partial wildcard/placeholder，或非末尾 `#`/alternatives。
- `deny` 顶层文档仍包含 topic block。
- alternatives 重复、少于 2 个、超过 16 个或不在末尾。
- 文档、topic、条目数或展开规则数超过容量限制。
