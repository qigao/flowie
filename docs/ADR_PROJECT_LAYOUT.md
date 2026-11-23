# 项目结构与构建归属

## 背景与影响

本次整理同时覆盖源码模块和 CMake。重构前有两处 MED 结构问题（事实）：核心库直接编译
`server/cluster/flowie_cluster.c`；Broker 使用的 `flowie_connection` 在客户端目录定义。
这使目录归属与实际依赖方向不一致，修改客户端构建文件也会影响服务端传输实现。
Control 的应用源码、可执行入口与测试注册集中在一个 CMake 文件，测试源码目录与构建作用域也不一致。

## 方案比较与选择

| 方案 | 收益 | 代价 |
| --- | --- | --- |
| 只把根目录 client/server 移到 flowie | 改动路径较少 | 保留核心依赖应用源码、连接实现归属客户端的问题 |
| 按能力归属整理源码，并保留现有 target | 目录、源码所有权与构建入口一致 | 需要同步源码路径、测试夹具和安装头文件路径 |
| 同时重写运行时接口与依赖注入 | 可进一步调整运行时边界 | 改变生命周期及错误语义，超出本次目录整理范围 |

选择第二项。所有一方产品源码放入 `flowie/`，模块内部按 `include/`、`src/`、`tests/`
区分公开头文件、实现和测试；没有公开 SDK 的应用模块只保留 `src/` 与 `tests/`。
schema、parser、assets、templates 保留独立资源目录。

- `protocol` 负责 MQTT 编解码；`client` 使用协议与外部网络 SDK。
- `transport` 拥有共享连接、发送批次、CPU affinity、执行绑定及 I/O 策略。
  `Flowie::Connection` 和 `Flowie::Transport` 仍是构建内部 target。
- `core/src/cluster` 拥有无外部集群 SDK 依赖的基础契约；`cluster/src` 拥有可选的
  TurboRaft/CNet 扩展。core 不再编译 server 应用目录的文件。
- `server` 与 `control` 组装应用。各自的测试在本模块 `tests/CMakeLists.txt` 注册。
- `flowie/detail` 只保存共享的私有头文件，不形成新库；`tests/support` 只保存测试夹具。

根 `CMakeLists.txt` 依次加载依赖、工具、vendor、产品模块、运行时复制和安装导出。
`flowie/CMakeLists.txt` 按 protocol、transport、client、core、可选 cluster、server、control
的顺序声明 target。`cmake/FlowieDependencies.cmake`、`FlowieRuntime.cmake`、
`FlowieInstall.cmake` 分别负责依赖查找、运行时文件和安装导出。

## 状态、接口与兼容性

本次不改变业务算法、线程模型、状态所有者、资源生命周期或错误码。
standalone 协议事实仍由 repository 拥有，可选集群事实仍由 TurboRaft 提交状态拥有。
目录迁移没有运行时状态迁移步骤，也没有数据转换或外部副作用。

公开 target `Flowie::Protocol`、`Flowie::Client`、`Flowie::Flowie`、平铺安装头文件名、
CLI、配置和容器部署入口保持不变。`flowie_execution.h` 源文件归入 transport，仍以原名安装，
core 的构建接口同时传递该头文件目录。SDK 发行版本见[根 README](../README.md#upstream-dependency-versions)。

LOW 兼容性影响（事实）：直接引用旧仓库源码路径的外部脚本需要更新路径；使用安装包 target 的
消费者无须调整 include。旧 build tree 需要重新 configure，生成器会重建发生路径变化的对象。
不搬移数据库、配置、部署资源或构建缓存。保留原有 legacy supervisor 源码，未扩大其构建范围。

## 验证与回滚

验证入口为 `native-release-user` 的完整构建图、正式 CTest 和 `cmake --install`。
目录迁移时的 Windows LLVM-MinGW 集群回归覆盖 CNet peer、容量、关闭、快照恢复和相邻模块。
当前 CI 的平台及覆盖范围见[发布要求](RELEASE_GATE.md)。

回滚只需反向迁移目录并恢复对应 CMake、CI 和文档路径，再重新 configure/build；不涉及数据回滚。
应保留同时存在的运行时修复，不能用重置整个工作区的方式撤销目录整理。

## Slate 本地构建入口

Windows x64 LLVM-MinGW 开发构建新增 Slate 入口，覆盖协议、传输、Client、Core、
Server、Control 和可选集群扩展。考虑过由 Slate 包装 CMake 与直接声明 Ninja 图。
采用模块自有的 Slate 清单，让 Ninja 跟踪生成文件、源码和链接输入，让 Slate 发现原有 TinyTest
测试。CMake 继续承担仓库的发布矩阵、安装导出和 Workbench。

两套清单共享源码、测试夹具与已恢复 SDK。当前 SDK 发布仅提供 CMake 元数据，因此
`presets/SlateDependencies.json` 声明已验证 ABI 所需的库、头文件、编译定义和 DLL。
升级 SDK 时需对照上游 CMake 导出核对这些声明。Flowie 自身发版时同步根
`CMakeLists.txt` 与 Core 清单的 Broker 版本。生成结果写入各自的构建目录，
共用现有 re2c、Lemon、salts-idlc 和 SQL 嵌入脚本。该构建入口没有运行时数据迁移步骤。

验证采用 Slate 的完整 Release build、模块测试、集群测试及 Ninja 增量构建。
用既有同 ABI CMake 产物比对 Client 与 Broker 的 PE 导出。回滚可删除 Slate 配置和
模块清单，随后用原 CMake preset 构建。
