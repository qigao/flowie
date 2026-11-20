# 构建与依赖排障

## 2026-11-18：Chttp 0.1.2 / TurboRaft 0.3.2

- `.env` 是非密钥配置的唯一指定源，`.env.example` 只作为模板。恢复脚本验证完整发布后，原子回写
  SDK 根目录，保留凭据、注释和其他机器设置。不要输出完整 `.env`。
- 当前配套：vcpkg-cache 0.1.3、SaltsBase 0.1.8、CMeta 0.1.8、SaltsUtils 0.1.6、
  SaltsCNet 0.1.16、Chttp 0.1.2、TurboDB 0.1.2、TurboRaft 0.3.2。
- vcpkg、re2c、Lemon 直接使用 `.env` 指定的共享恢复结果；Flowie 已删除本地 lock 副本。
  发布声明、摘要验证和解包归共享 vcpkg-cache，不在产品内重新实现下载器。

### TurboRaft 版本与所有权

事实：本地旧 stage 的 ConfigVersion 仍是 0.3.1，即使其 build cache 已写 0.3.2。
`find_package(... EXACT REQUIRED)` 正确拒绝了旧 stage。不要改 ConfigVersion 或消费未安装的 build tree。

0.3.2 发布没有 Windows LLVM-MinGW 归档。本次使用原仓库现有 native preset，在独立 build/stage
目录构建发布提交 `3e311ba3e23585a75c16cea1a5d462edcf332c09`；完整构建及正式 CTest 60/60 通过。
安装后才将新 `TURBORAFT_ROOT` 写入 `.env`，原仓库和旧 SDK 均保留。

Flowie 启用集群扩展后的完整构建、74/74 项启用 CTest 和正常安装通过；原有公网 MQTT live 测试
保持禁用。恢复脚本的正式 unittest 为 10/10 通过。

Flowie 使用 `TurboRaft::CNet`，由宿主拥有 CNet client、握手和 progress；runtime 拥有有界 peer 队列。
关闭时先停止接纳，再由宿主驱动 terminal callbacks。`destroy` 返回 EBUSY 时必须保留 runtime/generation，
不能提前释放回调引用的存储和投影。快照使用 0.3.2 的 snapshot source，recovery 拥有并释放 source。

### Windows PostgreSQL/MySQL 驱动加载失败

触发：`test_flowie_orm_runtime` 的 SQLite 检查通过，PostgreSQL 返回 `Plugin: load_failed`。
原因：SaltsCNet 0.1.16 的 `cnet.dll` 依赖独立 `salts_crypto.dll`；受限插件 DLL 搜索不会使用任意 PATH。

修复：在 `cmake/FlowieRuntime.cmake` 中将 `Salts::Crypto` 与 `Salts::CNet` 一起复制、安装到可信运行时目录。
保留插件加载的受限搜索策略。正式驱动回归随后通过。

TurboDB 本地源码完整构建另缺 Scintilla GUI 头文件，流程已停止，没有通过关闭应用模块绕过。
Flowie 最终使用 TurboRaft 0.3.2 发布清单指定的 TurboDB 0.1.2 官方 SDK，并通过正式驱动测试验证加载。

### 复验入口

在已配置共享根目录的 PowerShell 中执行：

```powershell
./cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Profile windows-llvm-mingw-x64 -Local
cmake --fresh --preset native-release-user -DBUILD_CLUSTER=ON -DFLOWIE_BUILD_CLUSTER=ON
cmake --build --preset native-release-user
ctest --preset native-release-user --output-on-failure -j 1
python -B -m unittest discover -s cmake/ci -p test_restore_native_sdks.py
cmake --install build/native-release-user
```

Windows 本地构建与回归不能证明 Linux、MSVC、Android 或远端集群部署通过；这些范围需各自验证。

## 2026-11-18：移除产品包分发与自有缓存代理

旧工作流仍浮动恢复已退役的 SDK 家族，并创建独立 consumer 工程。当前流程统一读取 job 的 `.env`，
复用本项目完整 presets/CTest，打包正常安装目录为原始 TAR，并生成 manifest 和 SHA256SUMS。
仓库自有 package client、缓存代理及其配置已移除；host tooling 的格式与恢复仍由共享基础设施拥有。

`setup_native_ci.py` 只编排共享 CLI：声明与生成锁保存在共享 checkout 的 `build/native-inputs`，
产品不复制版本锁或下载实现。它核对共享发布中的工具摘要，成功后才回写 `.env` 根目录。
本地已实际恢复到共享 `SHARED_DEPENDENCY_ROOT`，旧 SDK 与旧缓存目录保留。

CI 需配置 `FLOWIE_BUILD_ENV_LINUX`、`FLOWIE_BUILD_ENV_WINDOWS`、`FLOWIE_BUILD_ENV_ANDROID`
三个非密钥变量，由各 job 写成唯一 `.env`；不要把本机凭据或 Windows 路径复制到 Linux job。
Windows/Linux 运行正式测试；Android 只声明 cross-build、安装与 ELF 对齐检查。
工作流通过 actionlint，脚本 unittest 13/13 通过。托管 CI 与 Docker 构建仍需其运行环境验证。

## 2026-11-18：OpenWTL MQTT Workbench

`flowie/client/app` 现在通过 Windows 平台入口进入正式 build graph，使用公开
`Flowie::Client` 和 `OpenWTL::WTL`。GUI 依赖版本、SHA-256 由 `.env` 指定；
Windows CI 的 `FLOWIE_BUILD_ENV_WINDOWS` 也需补入 `.env.example` 中同名字段。
窗口、SDK 回调邮箱、异步关闭及容量协议见该模块的 `readme.md`。

触发：将原生窗口用例与模型用例链接到同一 TinyTest C++ 程序后，运行在
`ttest_enter_node__` 空配置处崩溃。LLDB 和头文件核对表明，OpenWTL 0.1.2
的兼容头定义 `_MSC_VER`，影响了后续 TinyTest 的 TLS 声明选择。
修复：先包含 TinyTest，再包含 OpenWTL；ATL base、WTL app、模块声明和
控件头也保留上游要求的顺序，用局部 clang-format 标记保护该顺序。
没有复制或修改上游实现。原生测试使用与应用相同的 common-controls v6
manifest，进程只初始化、终止一次 WTL module。

触发：直接启动 GUI 返回 `0xc0000135`，CTest 因 preset 的 SDK PATH 正常。
修复：构建与正常安装复制目标传递 DLL，以及所选 LLVM-MinGW target 的 C++
runtime。安装版已在 PATH 仅含 Windows 系统目录时成功启动，窗口响应正常。

实际验证：`cmake --fresh --preset native-release-user -DBUILD_CLUSTER=ON
-DFLOWIE_BUILD_CLUSTER=ON`、完整 preset build、正常 install 均通过；正式
CTest 75/75 通过（原有公网测试禁用）。应用专项通过 13 个用例、552 个断言，
包含真实窗口交互、MQTT 3.1.1/5、QoS 0/1/2、TXT/JSON/二进制回显、拒绝
认证/订阅、邮箱背压和异步关闭。SDK 脚本测试 13/13 通过。

本轮应用收发联调限本地 TCP 回环；未验证应用对外部 broker 的 TLS/WS/WSS
连接、MSVC 构建或 sanitizer。原生自动化接口不可连接，未完成截图视觉检查；
真实 Win32 控件交互已通过模块内正式 CTest，不能据此宣称视觉验收通过。

## 2026-11-18：多个 Publisher / Subscriber

工作台最多承载八个独立原生子窗口，各自拥有 Session、表单、订阅、历史和
滚动位置。通过显示/隐藏切换标签，不复制凭据或待处理命令；后台页继续收发。
子窗口仅在 SDK 清理完成后销毁，宿主随后释放对象。仅宿主在全部客户端清理后
发送 WM_QUIT；关闭单页不能退出整个程序。角色同时在控件和命令派发处限制。

触发：新增标签栏后的真实控件尺寸测试发现，系统缩放会让固定高度面板的
订阅列表与状态栏重叠。修复：客户端面板使用有界虚拟高度与原生垂直滚动，
启动窗口根据系统 DPI 和桌面工作区选择尺寸。正式测试实际缩小窗口、滚至底部，
核对控件不重叠且底部状态可达；保留每页滚动位置。

多连接路由 fixture 使用公开 Protocol 编解码。注意 packet envelope 的 version
保留调用方 parse options；初始 CONNECT 的协商版本应从 connect_parse 的 typed
view 获取，否则 UNSPECIFIED 会使后续 CONNACK 编码失败。

完整 native preset build、CTest 75/75 通过（原有公网测试禁用）；尺寸修复后
应用专项复验 18/18、610 断言通过。包含两发布端向两订阅端路由、后台收包、
不同 broker/topic 隔离、单页关闭后的继续收发、八连接容量、唯一默认 client ID、
空工作区重建、短窗口可达性及全窗口异步清理。正常 install 通过，安装版与当前
构建摘要一致，已在仅含系统目录的 PATH 下启动并确认窗口响应。
本轮多连接联调使用本地 TCP 测试 peer；未补做外部 broker/TLS/WS/WSS、MSVC、
sanitizer 或截图视觉验收。

## 2026-11-18：对齐 TurboDB Studio 的原生 UI

参考 `../turbodb/app/src/studio_colors.h`、命令栏和 splitter/notebook 组合，
采用深灰命令栏、白色内容、灰色分区标题、语义色线条图标与系统消息字体。
每个客户端包含左侧连接/订阅页，右侧可拖动的编辑、历史及原始负载窗格；
底部状态固定，短窗格独立滚动。WebSocket/TLS 字段按所选传输显示。
配色、绘制和 UiPanel 为私有 UI 实现，不引用兄弟仓库构建路径，也没有引入
Scintilla/Lexilla。Session、历史、凭据与退出协议继续由原客户端拥有。

原生 panel 将 WM_COMMAND/WM_NOTIFY/颜色通知同步转交 owner；通知中的借用指针
不能排入异步消息。父窗体只移动 splitter，panel 大小变化只重排内容；布局重入
与 SetScrollInfo 引发的尺寸通知有明确隔离。滚动范围使用实际像素高度，避免
DPI 往返舍入产生无意义的一像素滚动条。键盘焦点进入长表单时将字段滚入可视区。

触发：原生渲染图中的事件列没有语义色。修复：从 ListView 的实际 LVIS_SELECTED
读取选择状态，为每个 subitem 显式设置颜色，并按
[Win32 custom-draw 契约](https://learn.microsoft.com/en-us/windows/win32/controls/using-custom-draw)
返回 CDRF_NEWFONT；保留 Windows 自身的选择颜色。Win32/commctrl 和 ATL/WTL
头文件仍须按依赖顺序包含，用局部 clang-format 标记保护。

完整 native build 与 CTest 75/75 通过（原有公网测试禁用）；最终界面微调后
应用专项 19/19、620 断言通过。正式窗口测试生成三张实际控件 BMP，已转换为 PNG
并检查连接页、订阅页、命令焦点/按下状态及消息详情；产物位于
`build/native-release-user/flowie/client/app/tests/workbench-*.png`。
正常 install 与仅含系统目录 PATH 的安装版启动通过，窗口响应正常。
自动化 native pipe 仍不可用；本轮视觉核对依据实际窗口的 PrintWindow 渲染，
未验证 Windows 高对比度模式切换、MSVC 或真实鼠标拖动；布局和收发由正式用例覆盖。

标签关闭按钮使用原生 BUTTON，独立 HWND 绑定到客户端及 TCITEM.lParam，
不以会随删除变化的标签序号作为命令身份。布局取原生 TabCtrl 的实际 item rect，
并排除 overflow 箭头区域；按钮保留键盘焦点、辅助名称和 tooltip。关闭后台页
继续沿用 Session 异步清理，完成后才移除标签，不改变当前连接。应用专项
22/22、677 断言通过，覆盖后台关闭、重复点击、空工作区、溢出布局及 Ctrl+W；
实际窗口渲染图已核对标签关闭按钮与文字的间距。
最终完整 native preset build、CTest 75/75 与正常 install 通过（原有公网测试禁用）；
更新后的安装版在 PATH 仅含 Windows 系统目录时启动成功，窗口响应正常。

触发：原生 tab 绘制的灰白底色与其独立关闭按钮不一致。修复：两层 tab 使用
同一套 Studio palette 绘制背景、选中指示线和关闭按钮。绘制同时处理
WM_PAINT 和 WM_PRINTCLIENT，原生控件继续管理 item rect、选中、键盘与溢出；
应用为侧栏 tab 显式预留文字边距，避免标签实测被截断。分区、分隔条、表单按钮
和状态栏也引用主题颜色。正式应用测试 22/22 通过，已核对连接、订阅和命令焦点
三个实际窗口渲染图。未实机切换 Windows 高对比度。

## 2026-11-20：连接菜单、弹窗与历史

File 菜单和三个工具栏新增入口共用原生模态连接窗。用户确认 Connect 后才启动
连接，Open tab 只创建待配置的标签，取消不占用标签槽位。打开弹窗时其历史
列表持有快照，后台标签的 CONNACK 可以继续更新持久记录。

CONNACK 成功后仅将端点、传输、协议和角色从客户端交给宿主，在当前用户注册表
保存最近 12 条。默认不持久化凭据、客户端 ID、TLS 文件路径和 WebSocket 路径。
读改写以有界互斥保护同桌面会话中的实例，单一 REG_MULTI_SZ 值作为持久事实源。

触发：原生弹窗测试在提交后仍见到 HWND，误发取消导致所有新增命令返回取消。
修复：EndDialog 在模态消息循环恢复时销毁窗口，测试在提交后只根据真实的
校验错误决定是否取消。驱动使用同一 UI 线程的计时器，生产界面没有测试钩子。
实际测试覆盖三入口、取消、成功及拒绝、跨窗口复用、凭据留空、角色及 12 条上限。
完整 native preset build、CTest 75/75、正常 install 通过（原有公网测试禁用）。
应用专项 26/26、714 断言通过，更新版已启动且窗口响应正常。Computer Use
原生管道不可用，弹窗行为通过正式原生控件测试验证，未做实时截图核对。

## 2026-11-20：文件载入与标签任务视图

File 载入连接 JSON，Connection 管理新建、最近连接和断开，Message 载入
UTF-8 TXT、JSON 或带 topic/QoS/retain 的 `.mqtt-sample.json`，View 切换
Workspace、Publish、Monitor。已在模块 readme 给出文件样例和字段约束。

WTL Shell 文件选择器运行嵌套消息循环，后台连接仍会处理关闭。打开选择器前
记录目标窗口，返回时核对同一标签再写草稿，防止关闭标签后写入下一页。
文件校验结束后才修改表单，连接仍须通过确认窗，加载样例只编辑草稿。
任务视图使用 WTL splitter 单窗格模式，隐藏窗格原有状态及比例仍由原标签持有。

完整 native preset build、CTest 75/75 和 install 通过（原有公网测试禁用）。
已检查 Publish 和 Monitor 的原生窗口渲染，应用专项覆盖导入确认、错误时保留
草稿、显式发布、按角色限制和每个标签独立保留布局。

## 2026-11-20：本地 MQTT Collections

Collections 树使用 WTL TreeView，按集合和请求名分组。保存取活动标签的快照，
随后用模态窗选集合和命名，同名替换要求用户确认。窗口可继续处理后台会话事件，
因此保存完成后只按原 HWND 更新标签名字。打开请求创建新离线标签，已保存主题
仅回填 filter/QoS，订阅、发布和连接仍是独立命令。

存储上限为 64 条请求、8 MiB，每条最多 128 个主题。当前 Windows 用户的本地
JSON 文件为事实源，界面树持有已提交的排序快照。先写同目录临时文件并 flush，
再替换目标文件。独占锁文件使跨实例写入冲突可见，不覆盖另一个进程的更新。
保存时不读取密码字段，编码使用允许列表，排除用户名、client ID 和 WS 查询参数。
旧应用继续使用原 Recent Connections，Collections 可单独读取和恢复。

正式应用测试从实际保存窗写入隔离的文件，重新创建窗口并打开树项，核对凭据
留空、连接前无收发、连接后无自动订阅，以及手动订阅和发布可以回显。存储测试
覆盖拒绝未经确认的同名覆盖和验证失败时保留旧文件。原生渲染产物为
`build/native-release-user/flowie/client/app/tests/workbench-collections.png`。
完整 native 构建通过，应用专项 31/31、793 断言通过。首轮完整 CTest 中
Raft runtime 创建测试返回 SALTS_EPROTO，隔离复验及完整复跑均通过，
最终 75/75（原有公网测试禁用）。旧安装版仍在运行，新版独立安装到
`build/native-release-user/stage-collections`，以仅含系统目录的 PATH 启动成功。

## 2026-11-20：按 MQTT 请求组织工作区

参考 Postman MQTT 的信息层级，Collections 位于左侧，右侧从上到下为客户端
标签、固定 broker 连接栏、请求页签和消息流。请求页签为 Message、Topics、
Authorization、Settings，Publisher/Subscriber 按角色省略不适用页。采用
Flowie 原有 Studio 配色、OpenWTL 控件和 WTL 分隔窗格。没有新协议配置或存储迁移，
可回退界面实现并继续读取原有 Collections。

旧连接和发布控件移到各自的父窗格，每页保留独立滚动位置。短窗高下 WSS
Settings 的最后一行可滚动到可视区，连接动作和状态条保持可达。Publish 扩展
Message 页，Monitor 保留连接栏并扩展消息和载荷。筛选消息时维护可见列表行到
原历史的索引，详情和语义色读取同一记录，不修改原历史。
完整 native preset build、CTest 75/75、正常 install 通过（原有公网测试禁用）。
Workbench 专项 31/31、798 断言通过。已检查原生 Message、Topics 和 Monitor
渲染图。安装版以仅含系统目录的 PATH 启动成功，窗口响应正常。

## 2026-11-20：Workbench 多协议标签

Windows 客户端复用原有 OpenWTL 工作区和 Collections，新增 HTTP/HTTPS、JSON-RPC、
TCP、Serial。各标签持有独立 I/O owner 和运行记录。MQTT 旧收藏可直接打开。
新版收藏在写入其他协议后需要新版程序，回退时使用原 MQTT 收藏副本。

排查 CHttp 0.1.2 时，若 HTTP/RPC 还未拨号便在初始化失败，检查
`chttp_client_config` 的 `max_request_body_bytes`、`max_response_body_bytes`
和 `max_informational_responses`。发布版要求三项都非零，使用响应 sink
也需要设置。工具配置了 64 KiB 请求/响应上限和 8 个中间响应，随后用
真实回环验证响应头、响应体、JSON-RPC 关联以及取消时的 owner 清理。

通过 `cmake --build --preset native-release-user` 和
`ctest --preset native-release-user --output-on-failure -j1`。CTest 75 项通过，
原有公网 MQTT 测试禁用。Workbench 36 个用例、834 个断言通过。测试确认
HTTPS 开始 TLS 握手且对明文端点没有发送 HTTP 请求，Serial 错误路径可观察。
Workbench 的可信 HTTPS 响应和物理串口收发未做本机验证。正常安装到
`build/native-release-user/stage-workbench` 后，仅含系统目录的 PATH 下启动通过。

## 2026-11-20：Workbench 收藏文件与左侧导航

文件菜单和工具栏打开收藏 JSON。左栏的 Collections 按文件和收藏名称浏览；
History 使用现有 MQTT 最近连接；Manage 打开独立的文件管理标签。
显式加载和重新载入会先验证完整文件，再替换对应快照；卸载只移除工作区引用。
默认收藏沿用原路径自动打开，手动加载的文件归当前工作区会话。

复用 `style_tabs` 时，新增的导航标签若使用 common control 的默认 padding，
自绘的左右文本边距可能截断标题。参照请求页显式设置 `TabCtrl_SetPadding`，
并使用 Workbench 原生窗口测试输出的 `workbench-collections-manager.bmp`
核对标题、工具栏和选中态。完整 native preset build、CTest 75 项（原公网用例
禁用）、Workbench 38 个用例和 865 个断言均通过，正常安装至
`build/native-release-user/stage-collections/`。

## 2026-11-20：Workbench 全局请求命令

主菜单、工具栏和快捷键基于当前请求标签声明的动作与状态。各面板仍负责
命令准入和 SDK 生命周期。主操作按钮保留显示时对应的动作，执行时重新
验证可用性，避免 TCP 断开后把已经显示的 Send Bytes 误当成 Connect。
切换 Workspace、Editor、Output 只改变当前标签的可见窗格。

运行窗口用例时，回环端点收到请求与主窗口刷新工具栏不是同一个事件。
验证取消按钮需要同时等待请求已送达和按钮可用，随后检查连接关闭且
该标签仍存在。完整 native preset build、CTest 75 项（原公网 MQTT 用例
禁用）、Workbench 39 个用例和 890 个断言通过。串口覆盖不可用端口，
实物串口收发未验证。正常安装至 `build/native-release-user/stage-global-actions/`。

## 2026-11-20：托管 CI 配置与 MSVC Workbench 链接

GitHub Actions 运行 44665963477 和 44665969391 的三个平台都在生成 `.env`
时失败。仓库没有 `FLOWIE_BUILD_ENV_*` variables。可复用工作流现在用
`setup_native_ci.py --materialize <profile>` 从选定提交的 `.env.example` 生成
每个 job 的配置。它只复制发布版本与 Windows OpenWTL 字段，根据 profile
设置 triplet，并从 Android runner 取得 NDK 根。Windows runner 使用与
SaltsBase SDK 生产流程相同的 Windows 2025 镜像系列。

共享 vcpkg-cache 仍通过 `resolve-release` 和 `locked` 恢复安装包。
re2c 和 Lemon 从同一公共发布取已校验的归档，再交给共享
`package <id> <version> --archive` 入口验证身份并解包。此前的仓库变量配置
已由模板生成替代。

实际 MSVC 构建还发现 `CVT1100` 和 `LNK1123`。同一 manifest 同时由
`workbench.rc` 和 CMake 链接输入嵌入。现在由 RC 单独嵌入，并给应用及测试
的 RC 对象显式声明 manifest 构建依赖。用正式 `ci-msvc-release-user` preset
重新配置、构建、CTest 和安装，53 个 CTest 项通过，原公网 MQTT 用例禁用。
其中 Workbench 39 个用例、890 个断言通过，安装版资源 1 已核对包含
Common Controls v6。脚本 18 个用例和 actionlint 通过。Linux 和 Android
托管构建仍待修改后的工作流运行验证。
