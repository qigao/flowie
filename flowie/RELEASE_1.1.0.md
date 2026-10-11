# Flowie 1.1.0 发布准备

状态：候选版本，尚未创建 tag 或发布包。上一正式版为 1.0.1。

## 主要变化

- TLS 与 JWT 加密依赖迁移到 GmSSL / Salts Crypto，移除旧 OpenSSL/BoringSSL 适配。
- Server 使用 ACE 静态 Component Configurator 装配 Security、Repository、Endpoint，
  提供反向清理和失败回滚；YAML/CLI 配置与 `--check` 共用配置装配。
- HTTPS Auth/ACL 通过 typed interceptor 统一请求校验、结果处理和敏感数据清理。
- CNet Server 组合 Manager/Handoff 与连接分配策略；Client 通过 ManagedDial 和目的地址策略
  保留逻辑 TLS authority，只有完整 MQTT CONNACK 协商成功才进入 ready。
- 修复客户端重连凭据、未读接收数据、命令期限、回调停止、关闭重试及原生 owner 生命周期边界。
- 更新内置控制台模板与模型；使用 Salts 的 import-safe tstr 描述符解决 MSVC 静态初始化限制。
- 新增 Linux CTest 空闲 CPU / 独立入站延迟 benchmark，覆盖 TCP/TLS/WS/WSS。

## 兼容与升级

本轮准备不修改部署域名、用户数据或 password file。现有自建部署依赖的 TidesSQL 继续关闭；
已检查 [TurboDB 2.3.4 官方包](https://github.com/qigao/turbodb/releases/tag/v2.3.4)：
五个平台均不再包含 TidesDB/TidesSQL，相关 CMake 依赖查找与目标导入关闭。
Flowie 源码构建及安装包要求 TurboDB 至少为 2.3.4，依赖恢复仍选择最新发布版本。
升级应恢复到新的 SDK 目录，避免旧包文件残留。
GmSSL 与 CMeta/CNet SDK 属于新的原生依赖组合；消费方应重新构建并一起更新相关 SDK，
不能将新头文件与旧 OpenSSL 时代的二进制混用。具体 producer 版本以候选包 manifest 为准。

已有 Linux 源码验证使用 Salts 2.3.0-rc.7、SaltsUtils 4.3.0-rc.8、CHttp 2.1.0-rc.3；
原生 CI 已选择 Salts 2.3.0-rc.10、SaltsUtils 4.3.0-rc.8、CHttp 2.1.0-rc.3。
这两组验证输入不同，不能混作同一个包的资格证明。

## 已有证据与候选验证

- 集成提交 `a76dd6b`：Linux 源码全量回归 52 项通过，公网 live 项关闭。
- [原生 CI 38101610086](https://github.com/qigao/flowie/actions/runs/38101610086)：
  Linux 成功；Windows 在 dashboard tstr 静态描述符处编译失败，本候选修复后须重跑。
- benchmark：12 组通过，共 6144 条消息，未发生 admission retry。
  空闲 CPU 为单核的 0.706%–1.336%，各组入站 P99 为 1.756–7.158 ms。
  这是远程 VM 上独立 peer 经 loopback 到客户端回调的测量，包含服务端调度、队列、TLS/WS 与解析成本，
  不是纯 CNet wake 延迟，也不是生产或最坏情况保证。复现方法见 [Client Guide](CLIENT_GUIDE.md#10-空闲-cpu-与独立入站测量)。
- 1.1.0 候选的精确 SHA 原生回归、三平台 SDK 打包及安装后消费验证：待完成。
- [TurboDB 2.3.4 原生回归 38116979331](https://github.com/qigao/flowie/actions/runs/38116979331)：
  验证提交 `a0a60a706a9d66c3e9c7fc375233d2e563e07650`，Windows/Linux 均确认恢复 2.3.4。
  Linux 52 项通过、1 项公网 live 关闭，安装后的 C11/C++17 测试 2/2 通过。
  Windows 编译通过、50 项测试通过、1 项失败、1 项公网 live 关闭；两平台 ORM 测试均通过。
  原生 CI 的安装后测试使用版本化 user presets，并启用根目录 vcpkg manifest。

## 发布前阻塞与范围

- HIGH：上述 2.3.4 原生回归中，Windows 的 `test_flowie_control_runtime` 仍在
  `composes JWT JWKS auth for management and broker endpoints` 用例中崩溃，
  安装后消费步骤未执行。此问题也曾出现在 TurboDB 2.3.2 的
  [原生 CI 38105842320](https://github.com/qigao/flowie/actions/runs/38105842320)，
  尚无证据确定崩溃根因。
- 已解除：TurboDB 2.3.4 包排除了 TidesDB/TidesSQL，下载包的三项正式包测试通过，SHA-256 为
  `cb47e45a07c80bbc6ffaa9a6aa06f50c4c24f1bce006caba73785e69e4889aba`。
  producer 为 Salts 2.3.0-rc.10 / SaltsUtils 4.3.0-rc.8。
  ORM 正式测试移除了已退役驱动的成功加载预期，保留缺失模块错误传播验证；
  Flowie 两平台的 ORM 测试已通过；其他回归结果见上。
- HIGH：[SDK qualification 38105843587](https://github.com/qigao/flowie/actions/runs/38105843587)
  在 Linux Cluster 链接失败：已发布 TurboRaft 0.3.1 仍引用 `salts_fs_write`、旧
  `tbe_typed_*` 等符号，并引入 `libcrypto.so`，不兼容当前 Salts/GmSSL 组合。
  Windows 与 Android SDK 构建成功，但 Pack 与安装后消费步骤因 Linux 失败未运行。
  需要兼容的 TurboRaft 发布包；保留 Cluster 验证，不通过关闭它掩盖失败。
- HIGH：目前没有同一候选 revision 的完整产品 gate 证据。
  [RELEASE_GATE.md](RELEASE_GATE.md) 中部分自动门禁实现已缺失；固定 broker、持久化、mTLS、
  sanitizer 与长稳结果不能由普通 CI 或此前部署测试代替。
- MED：SDK pipeline 在正式 tag 上会重新构建，并重新解析 producer 版本；准备包与最终包
  不能假定相同，最终发布须重新核对 SHA、依赖 manifest 与工件哈希。

静态 YAML Configurator 已实现；动态 Plugin/hot reload、连接池、共享非阻塞进度驱动与
多尝试 CNet 恢复策略不属于本版本已验证能力。Android 资格验证是编译链接，不代表设备运行测试。

## 准备与发布步骤

1. 对候选提交运行 `Flowie native tests` 与 `Flowie native SDK qualification`。
   手动 qualification 选候选分支，记录 run 的精确 head SHA。
2. 收集 `Flowie.Native.1.1.0` 工件及各平台 manifest，检查版本、source、依赖版本和验证范围。
3. 补齐上述产品门槛证据；由发布者评估所有剩余阻塞，不能静默降低要求。
4. 获得发布授权后，对已验收 SHA 创建 `v1.1.0`。推送该 tag 会自动触发构建与发布，
   不应为了测试流程提前推送 tag，也不覆盖已有版本。
5. 核对发布 run、GitHub Release、NuGet 包及其哈希；部署另按原数据和配置的升级流程执行。
