# Flowie-Control JWT/JWKS 认证

## 决策背景

Flowie 的 MQTT 数据面只负责协议收发，认证与 ACL 事实源归 Flowie-Control。现有
`external_authenticator` 抽象可以接入外部身份，但此前 assertion 不携带 Domain，运行时因此
有意拒绝启用外部认证。JWT/JWKS 若绕过该限制，会允许同名 subject 被映射到调用方声明的任意
Domain，形成跨租户主体混淆。

本设计先将 Domain 绑定纳入可信 assertion，再增加 JWT/JWKS provider。Flowie 仍通过现有
`/v4/authenticate` 调用 Flowie-Control，成功认证后再独立调用 ACL；JWT 声明不能直接授予本地
角色或 ACL。

## 候选方案

1. 复制 `TurboHTTP/vendor/cjwt` 到 Flowie。实现直接，但会产生第二份版本、安全补丁和许可证
   事实源，拒绝采用。
2. 每次认证远程调用 IdP introspection。密钥轮换简单，但每次认证都增加网络依赖；仓库已有
   `external_https` provider 承载这一模式。
3. 通过已安装的 `TurboHttp::Cjwt` 验证 JWT，并从配置的 HTTPS JWKS endpoint 刷新公钥。
   本方案采用此方式：签名验证本地完成，密钥和工作量均有硬上限。

## 信任边界与声明契约

- MQTT 客户端提交的用户名、认证方法、token、协议和远端地址均不可信。
- Flowie 到 Flowie-Control 的调用身份继续由现有 service credential/mTLS 边界验证。
- 只接受配置指定的一个非对称算法；不接受 `none`、HMAC 算法，也不读取 token 的 `jku`、
  `jwk`、`x5u` 或 `x5c` 来改变信任源。
- JWT header 必须含非空 `kid`；JWKS 中必须存在且只存在一个同名 key。key 的 `use` 必须是
  `sig`，`alg` 必须与配置和 token header 三者完全一致，key type 必须与算法族匹配。
- JWKS 拒绝对称 key、私钥参数、重复 `kid`、缺失元数据、超出 key 数量上限或大小上限的文档。
- 必须验证签名以及 `iss`、`aud`、`sub`、`exp`、`nbf`、`iat`、`domain_id`、
  `account_enabled`、`revision` 和 `assurance_level`。`exp`/`nbf` 的允许偏差来自配置；`iat`
  不得晚于当前认证时刻。
- `sub` 必须与请求的 presented identity 完全一致。Broker 通用认证不把 service principal 的
  Domain 当作业务 Domain；签名后的 `domain_id` 是后续本地映射的目标。管理登录等已知目标 Domain
  的入口会把它作为 expected Domain，并要求 assertion 精确匹配。
- JWT group 仅是外部映射输入，不能直接复制为本地授权。最终 principal、角色、组和 policy
  version 仍从当前 Domain 的 TurboDB repository 快照读取。

## 数据与并发协议

权威状态是 authenticator 拥有的一份不可变、已验证 JWKS snapshot。HTTP response body 只是候选
输入；只有完成 JSON/JWK 结构检查后才能在写锁下替换 snapshot。认证 worker 在读锁内借用当前
snapshot，返回后借用失效。不存在第二份可独立推进的 key cache。

网络 fetch 在当前 CoroNet owner lane 的 coroutine 中发起，使用 HTTPS、严格 peer verification、
禁用 redirect/retry，并限制 timeout、header 和 body。JWKS 解析及每次签名验证提交到 provider
拥有的有界 `turbo_threadpool`，不会在 owner lane 执行重型密码学函数。

任务 payload 由 job 自己复制并拥有；提交失败时创建方释放，提交成功后 worker 和等待 coroutine
各持一个引用。队列满立即返回 `TURBO_EBUSY`，等待超时返回 `TURBO_ETIMEDOUT`；worker 可安全
完成并释放最后一个引用，不借用已返回 HTTP request 的内存。

一个 authenticator 允许多个认证 producer 和多个 worker consumer。JWKS 到期时仅一个请求成为
refresh owner；没有未过期 snapshot 的其他请求 fail closed。刷新失败不会延长旧 snapshot 的有效
期。关闭顺序是：停止 HTTP admission、销毁 endpoint、shutdown/drain worker pool、销毁 snapshot
与锁、最后释放 authenticator。

## 配置与运行行为

`auth.jwt_jwks` 与 `auth.external_https` 互斥。JWT/JWKS provider 包含自己的验证 executor，因此
endpoint 不再套第二层 executor。未配置该块时，本地密码认证的配置和行为不变。

JWKS 初次按需拉取。未取得有效 snapshot、snapshot 到期、未知 `kid`、签名或声明不匹配均拒绝
认证；不会回退到本地密码、旧 key 或远程 introspection。

## 迁移、回滚与验证

迁移时先让 IdP 发布带完整元数据的公钥，并签发包含上述必需声明的 token，再启用
`auth.jwt_jwks`。密钥轮换必须让旧、新公钥在 JWKS 中重叠至少一个 refresh interval 加最大 token
寿命。

回滚仅需移除 `auth.jwt_jwks` 并恢复原认证配置；该功能不迁移或修改业务数据。TinyTest 覆盖配置解析、
claim、时间窗口、错误签名、跨 Domain、key 元数据、初次 HTTPS 拉取与失效 snapshot；队列饱和、
刷新竞争和 shutdown 故障注入仍属于上线前压力测试范围。
