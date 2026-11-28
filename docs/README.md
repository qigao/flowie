# Flowie 文档

Flowie 是 MQTT 协议与业务处理层。standalone MQTT 协议事实通过 `Orm::C` 的 typed
repository 管理；server/cluster 的数据与日志统一由 TurboRaft log/state-machine snapshot 管理；Graph 业务
事实由产品自己的 sink/repository 管理。三者不得共享事实归属、连接、migration 或 fallback 路径。
Flowie 内部 owner/route 容器只保存可由 committed Raft log 或 snapshot 重建的投影。

认证与上述存储边界完全分离：bundled Flowie 只配置 HTTPS auth provider；`flowie-control` 可选择
Repository 本地 Auth 或第三方 HTTPS Auth。第三方 credential、OIDC、LDAP/AD、RADIUS 或身份数据库
都留在第三方认证服务内部。TurboDB ORM、session store 和 Graph adapter 都不是认证来源。规范性决策见
[HTTPS 认证服务设计](ADR_HTTPS_AUTH_SERVICE.md)，部署配置见
[Flowie Control 部署与配置](CONTROL_GUIDE.md)。

---

## 文档导航与分类

### 1. 正式核心与指南文档 (Official Guides & Manuals)

- [架构说明](ARCHITECTURE.md)：Flowie 架构设计、分层模型与核心流程
- [配置式 Broker 概念与术语](CONFIGURED_BROKER_CONCEPTS.md)：Flowie 概念定义、模式与术语字典
- [服务端使用指南](SERVER_GUIDE.md)介绍 `flowie_server` 的 CLI、YAML 预检、监听器和协议存储
- [IoT 网关部署](../deploy/server/README.md#iot-网关部署)介绍 Domain Configuration、Domain Packages、设备认证与下游接入
- [Flowie Control 部署与配置指南](CONTROL_GUIDE.md)：控制面 `flowie-control` 架构、部署与安全配置
- [第三方系统接入指南](THIRD_PARTY_INTEGRATION.md)：Domain、管理 API、service credential、MQTT 客户端与 ACL 接入流程
- [Flowie Control ACL 文法与使用](ACL_GRAMMAR.md)：Role/Group/User ACL、通用 topic filter 与发布流程
- [客户端开发指南](CLIENT_GUIDE.md)：Flowie MQTT 客户端 SDK 接口与集成
- [Windows Workbench](../flowie/client/app/readme.md)介绍 MQTT 和其他协议的桌面测试工具
- [容器部署](../deploy/server/README.md)介绍 Broker、Control 和 PostgreSQL 的 Compose 部署
- [SDK 资格与产品发布要求](RELEASE_GATE.md)列出当前自动化与产品验收的范围

### 2. 架构决策记录 (Architectural Decision Records - ADRs)

- [项目结构与构建归属](ADR_PROJECT_LAYOUT.md)：源码模块、内部边界、CMake 组装与目录迁移兼容性
- [Chttp App 与共享 JWT](ADR_CHTTP_APP_INTEGRATION.md)记录 HTTP/WebSocket 迁移、回调所有权与配额
- [ACE 与 CNet 集成](ADR_ACE_CNET_23_SERVER_CLIENT.md)记录 TCP/TLS owner、handoff 和静态组件装配
- [Control 与 Domain 归属](ADR_AUTH_DOMAIN_SERVICE.md)记录权限模型和 Broker/Control 部署边界
- [MQTT 集群归属](ADR_MQTT_CLUSTER_OWNERSHIP.md)记录实验集群的提交、快照与恢复边界
- [HTTPS 认证服务设计](ADR_HTTPS_AUTH_SERVICE.md)：HTTPS Auth Provider 信任边界与 API 规范
- [Domain Packages 与网关激活](ADR_DOMAIN_RELEASE.md)记录签名包、身份与授权边界及 RulesForge 投递
- [分层 ACL 主体与通用 MQTT Topic](ADR_SUBJECT_SCOPED_MQTT_ACL.md)：Role/Group/User 共享规则、deny veto、Domain 隔离与兼容决策
- [动态 ACL Bundle 设计](ADR_DYNAMIC_ACL_BUNDLE.md)：版本化 ACL Bundle 格式、发布机制与同步
- [控制面启动参数决策](ADR_CONTROL_STARTUP_OPTIONS.md)：`flowie-control` CLI / Environment / DotEnv 优先顺序与配置隔离
- [控制面凭据管理与哈希](ADR_CONTROL_CREDENTIALS.md)：Argon2id 散列算法、参数与凭据存储规范
- [控制面类型化规则存储](ADR_TYPED_SUBJECT_RULE_STORAGE_RPC.md)：TurboDB 唯一持久化边界与 Role/Group/User 规则 RPC
- [协议数据与业务数据存储分层](ADR_PROTOCOL_BUSINESS_STORAGE.md)：ProtocolStore / TurboDB ORM 边界与迁移

### 3. 测试与运维手册 (Test & Ops Runbooks)

- [MQTT 协议测试矩阵](MQTT_TEST_MATRIX.md)：MQTT 3.1.1 / 5.0 协议特性测试用例与验证覆盖矩阵
- [管理 JSON-RPC API](MANAGEMENT_RPC_API.md)列出方法契约和错误语义
- [Control JWT/JWKS 认证](FLOWIE_CONTROL_JWT_JWKS_AUTH.md)说明认证配置和信任边界
- [固定 Mosquitto 互操作环境](../interop/mosquitto-2.0.22/README.md)说明测试 broker 和证书配置

### 4. 关联组件与底层设计 (Related System ADRs)

- [TurboDB ORM 统一状态、索引、日志与时间序列](ADR_TYPED_ORM_PERSISTENCE_V2.md)
- [实验集群运行时](FEATURE_TURBORAFT_CNET_CONTROL_PLANE.md)
- [服务端网络与容量配置](SERVER_GUIDE.md#tcptlsudpkcpws-与-wss)

### 5. 历史验证记录

- [长期运行日志分析](FLOWIE_LONG_RUNNING_LOGGING.md)
- [Flowie 与 Mosquitto 的 QoS 实现对比](MQTT_QOS_COMPARE_FLOWIE_VS_MOSQUITTO.md)
