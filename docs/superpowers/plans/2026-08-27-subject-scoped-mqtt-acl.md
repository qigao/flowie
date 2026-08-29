# Subject-scoped MQTT ACL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Control ACL 从固定的 `user` + `groups/.../devices/...` 文档，扩展为按 `role`、`group`、`user` 归属且支持受限通用 MQTT filter 的规则，同时保持既有 user 文档兼容。

**Architecture:** 继续以 ACL 文档为草稿事实源，由 parser 生成类型化 subject 与规范化 topic 元数据，Control 仓储在提交/发布边界校验主体，compiler 生成既有 `flowie_security_rule_t`。运行时沿用现有 role/group/principal 匹配与全局 deny veto，不改变安全 C ABI、发布 bundle 格式或 `/v4/acl/check`。

**Tech Stack:** C11、re2c、Lemon、SQLite、libpq/PostgreSQL、TinyTest、Mustache、原生 JavaScript、CMake Presets。

**Spec:** `flowie/ADR_SUBJECT_SCOPED_MQTT_ACL.md`

## Global Constraints

- 旧的规范化 `user` ACL 文档必须继续解析、格式化并编译为相同规则。
- subject 只能是 `role`、`group`、`user`；不对管理文档开放内部 `ANY`。
- topic 必须以精确 domain 段开头，并至少包含一个资源段；`+`、`%u`、`%c` 只能占据完整段，`#` 只能是末段，枚举只能是末段。
- 资源 topic 中名为 `groups` 或 `devices` 的段不再引用 Control Group；只有类型化 `group` subject 和成员关系具有组织语义。
- SQLite 与 PostgreSQL command 实现保持同语义；任何一侧失败都不允许静默降级。
- 每项行为先增加失败测试并确认失败原因，再实现最小代码使其通过。

---

## Task 1: Subject 类型与通用 topic 的 parser/compiler 契约

**Files:**

- Modify: `control/tests/test_flowie_control_acl.c`
- Modify: `control/flowie_control_acl_internal.h`
- Modify: `control/parser/acl_grammar.y`
- Modify: `control/parser/acl_lexer.re`
- Modify: `control/flowie_control_acl.c`

- [x] 在 `test_flowie_control_acl.c` 增加 role/group/user 三类文档的解析与规范化测试，断言 `document.subject_kind`。
- [x] 增加通用 topic 测试，覆盖任意静态段、`+`、`#`、`%u`、`%c` 与末段枚举，并断言旧固定路径仍原样规范化。
- [x] 增加拒绝测试：未知 subject 类型、空段、domain wildcard、段内 wildcard/placeholder、非末段 `#`、非末段枚举、重复枚举、超限枚举。
- [x] 构建并运行 `test_flowie_control_acl`，确认新增测试因当前固定 grammar 失败。
- [x] 给 ACL document 增加 `flowie_security_subject_kind_t subject_kind`；删除仅服务于资源路径 Group 引用的 offsets/lengths 元数据。
- [x] 修改 lexer/grammar，使 subject keyword 解析为类型化 kind，并把 topic 交给单一有界校验入口。
- [x] 在 `flowie_control_acl.c` 实现段级 topic 校验和枚举计数；复用当前 bounded buffer 与错误码，不引入动态分配。
- [x] formatter 输出对应的 `role`、`group` 或 `user` keyword；compiler 将类型原样写入 `flowie_security_rule_t.subject_kind`。
- [x] 运行 `test_flowie_control_acl` 直到通过，并检查旧 user 编译结果没有变化。

## Task 2: 公共语法校验与 SQLite 仓储语义

**Files:**

- Modify: `control/tests/test_flowie_control_store.c`
- Modify: `control/flowie_control_validation.c`
- Modify: `control/flowie_control_store.c`

- [x] 增加 store 测试：存在且启用的 role/group/user subject 均可写入草稿和发布。
- [x] 增加测试：不存在或禁用的对应 subject 返回明确错误；相同类型重复 subject 被拒绝，不同类型同名 subject 可并存。
- [x] 增加测试：通用 topic 不要求其中静态段对应数据库 Group；topic domain 不匹配仍被拒绝。
- [x] 增加引用测试：draft/published 的 role、group、user subject 阻止对应实体删除/禁用；普通 topic 段不形成 Group 引用。
- [x] 运行 `test_flowie_control_store`，确认新增测试在旧的 principal-only 校验上失败。
- [x] 将 domain 前缀检查改为首段精确匹配，保留 canonical document 检查。
- [x] 按 `document.subject_kind` 查询 user/role/group 的存在和 enabled 状态，并把唯一键改为 `(subject_kind, subject)`。
- [x] 删除从 topic 路径推导 Group 引用的逻辑；draft 引用仅来自类型化 subject，published 引用继续读取编译规则。
- [x] 运行 `test_flowie_control_store` 与 `test_flowie_control_repository`，确认事务、revision、发布 bundle 和错误传播保持稳定。

## Task 3: PostgreSQL command/query 语义对齐

**Files:**

- Modify: `control/tests/test_flowie_control_pgsql_database_live.c`
- Modify: `control/flowie_control_pgsql_command.c`
- Review/Modify if required: `control/flowie_control_pgsql_query.c`

- [x] 在 live 测试增加 role/group/user 草稿、同名不同类型、通用 topic 和类型化引用场景。
- [x] 在配置了 live PostgreSQL 测试环境时运行目标，确认旧实现失败；环境未配置时记录跳过条件。
- [x] 将 PostgreSQL subject 存在/启用校验、唯一键和 draft 引用扫描与 SQLite 实现对齐。
- [x] 删除 PostgreSQL 从资源 topic 推导 Group 引用的逻辑，保持 query/bundle load 使用 compiler 的类型化结果。
- [x] 运行 PostgreSQL unit/live 相关目标；对无法执行的 live 范围保留明确风险说明。

> 验证记录：本机未设置 `TURBO_FLOW_PGSQL_TEST_CONNINFO`，且当前 CMake 未注册 PostgreSQL live
> target；已对 command、query 和 live test 三个翻译单元执行 MSVC `/Zs` 语法检查。

## Task 4: Dashboard 类型化 ACL 编辑器

**Files:**

- Modify: `control/tests/test_flowie_control_dashboard.c`
- Modify: `control/flowie_control_dashboard_view.c`
- Modify: `control/templates/dashboard_content.mustache`
- Modify: `control/assets/control.js`

- [x] 增加 Dashboard 渲染测试，断言规则行包含 subject kind 标签，编辑器提供 role/group/user 类型和对应候选数据。
- [x] 运行 `test_flowie_control_dashboard`，确认当前 user-only UI 失败。
- [x] view model 输出 subject kind/label，并继续复用已有 user/group/role option 数据。
- [x] Mustache 编辑器增加 subject type 选择；候选选择按 type 切换，文案改为主体 ACL，topic 示例改为通用业务路径。
- [x] JavaScript parser/builder 支持三类 canonical 文档，并在切换类型时生成相应 keyword。
- [x] 运行 Dashboard 测试，并人工核对 add/edit 的 canonical hidden `rule_line`。

## Task 5: 文档、兼容性和实施状态

**Files:**

- Modify: `flowie/ADR_SUBJECT_SCOPED_MQTT_ACL.md`
- Modify: `flowie/README.md`
- Modify: repository ACL examples/docs discovered through `rg.exe`

- [x] 更新用户可见 ACL grammar、role/group/user 示例、继承闭包与 deny veto 说明。
- [x] 明确旧固定 topic 是兼容子集，不再赋予 `groups`/`devices` 组织语义。
- [x] 在代码与测试完成后将 ADR 状态改为已实施，并记录无需 schema/C ABI/protocol migration。

## Task 6: 分层验证

- [x] 用 `win-dev-user` configure/build preset 构建 ACL、store、Dashboard 和相关 PostgreSQL 测试目标。
- [x] 运行最小测试：`test_flowie_control_acl`、`test_flowie_control_store`、`test_flowie_control_dashboard`。
- [x] 运行相邻回归：repository、management service/RPC、auth/ACL security-contract 测试。
- [x] 按可用环境运行 PostgreSQL live 测试；若环境缺失，记录确切跳过依据。
- [x] 运行 `git diff --check`、检查 `.codegraph/` 未进入提交范围，并复核最终 diff 只包含本任务文件。
- [x] 仅在所有可执行验证通过后报告完成，列出未执行范围和剩余风险。
