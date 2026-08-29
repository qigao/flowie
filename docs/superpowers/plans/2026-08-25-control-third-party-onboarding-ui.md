# Control Third-Party Onboarding UI Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 让 Control Dashboard 能清晰引导 `system/admin` 建立第三方 Domain，并为 human 管理员显式创建或替换登录密码，同时继续复用既有 Domain/User/Role/Password Management command。

**Spec:** `flowie/THIRD_PARTY_INTEGRATION.md`

**Architecture:** Dashboard 只增加现有命令的可视化入口，不引入五步复合写入接口。Domain、User、Password、Role 仍分别提交、分别审计、失败即停；服务 token 与 human 密码继续使用不同 UI 和命令。密码表单由 Dashboard action 适配为 `flowie_control_management_password_set`，并在释放前擦除解码后的密码字段。

**Tech Stack:** C11, Iris HTTP, Mustache, HTMX, TurboUtils, TinyTest

---

### Task 1: 固化 human 密码 UI 与第三方接入引导契约

**Files:**
- Modify: `control/tests/test_flowie_control_dashboard.c`
- Test: `control/tests/test_flowie_control_dashboard.c`

1. 增加 system admin 渲染测试：第三方接入引导可见；human 用户显示密码入口；service 用户只显示 token 入口。
2. 增加表单测试：`create` 设置首密码成功，确认密码不一致和未知 mode 失败，`replace` 显式替换成功。
3. 运行聚焦测试并确认因缺少 UI/action 路由而失败（RED）。

### Task 2: 实现最小 Dashboard UI 与 action 适配

**Files:**
- Modify: `control/flowie_control_dashboard_view.c`
- Modify: `control/templates/dashboard_content.mustache`
- Modify: `control/assets/control.css`
- Modify: `control/flowie_control_dashboard.c`
- Test: `control/tests/test_flowie_control_dashboard.c`

1. 在用户模型中标记 human principal，并仅向有 `security_admin`/`system_admin` 权限者显示密码操作。
2. 在 Overview 增加第三方平台接入步骤，引导创建 Domain、切换作用域、创建 human 用户、设置密码、创建并分配角色。
3. 增加 human 密码 popover，要求显式选择 `create` 或 `replace`、输入和确认至少 16 字节密码；不显示或生成密码。
4. 在 Dashboard action 中严格校验字段、mode、长度与确认值，调用既有 `flowie_control_management_password_set`，并在销毁表单前擦除秘密字段。
5. 运行聚焦测试并确认通过（GREEN）。

### Task 3: 同步操作文档与回归验证

**Files:**
- Modify: `flowie/CONTROL_GUIDE.md`
- Test: `control/tests/test_flowie_control_dashboard.c`

1. 说明 Dashboard 中的第三方接入入口、human 密码 `create/replace` 语义，以及 service token 的一次性返回语义。
2. 修正仓库内 operator 脚本示例路径，使文档命令可直接从仓库根目录执行。
3. 运行 Dashboard 聚焦测试、相邻 Management service/RPC 测试以及格式/静态检查；记录因环境无法执行的验证和残余风险。
