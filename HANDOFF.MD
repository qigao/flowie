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

## 2026-11-20：Windows 短路径与 Android 宿主生成器

托管 Windows runner 的临时目录可能经由 `RUNNER~1` 提供。SDK 脚本有意
返回规范路径，测试应解析预期路径后比较，而不是比较原始字符串。

Android 交叉构建在 Ubuntu 执行 `salts-idlc` 生成 Control SQL。Android
SDK 中的同名程序是 AArch64 目标文件，不能用于这一步。恢复过程同时选择
同一发布的宿主 SaltsUtils 和 SaltsBase，并通过 `SALTS_IDLC_ROOT` 与
`SALTS_IDLC_BASE_ROOT` 交给生成命令。宿主生成器及其运行库独立于目标链接。

运行 44671000258 的 Windows Python 检查已通过，Android 已完成构建与安装。
Android 打包检查发现 NDK r27 产出的 `libflowie.so` 未达到 16 KiB ELF
段对齐要求。Android 工具链 preset 为可执行文件和共享库显式指定
`max-page-size=16384` 与 `common-page-size=16384`，由打包检查验证产物。

运行 44671306405 的 Linux 和 Android SDK 已打包，Windows Python 检查和
构建已通过。Windows Workbench 回归在 900 DIP 窗口本地重现：新增标签先
滚入可见区域，随后状态标题增加宽度，使关闭按钮被裁剪。更新标题后再选中
标签，保留原生滚动行为。传输下拉框测试使用控件报告的行高和选项数，避免
受固定像素阈值影响。两处修复通过本地 `ci-msvc-release-user` Workbench CTest。

## 2026-11-20：原生客户端 SDK 矩阵

发布配置来自 `cmake/ci/setup_native_ci.py --matrix`。它为 Windows x64/arm64
选择 MSVC 和 LLVM-MinGW，为 Linux x64/arm64 选择 GCC 和 Clang，为
macOS arm64 选择 Clang，为 Android x86_64/arm64-v8a 选择 NDK Clang。
归档步骤复用同一配置，逐一核对 profile、源码 SHA 和原生 CTest 状态。

Windows ARM64 的 MSVC 和 LLVM-MinGW 分别使用 ARM64 vcpkg triplet。LLVM-MinGW
的原生 preset 清除从交叉编译 preset 继承的 CMAKE_SYSTEM_NAME 后在 ARM runner
执行 CTest。归档检查客户端库和 Workbench 的 PE 机器类型，避免把 x64 产物
标成 ARM64。本地使用现有 MSVC 和 LLVM-MinGW 安装产物确认两种 x64 包通过，
并在将其标为 ARM64 时被拒绝。

共享 LLVM-MinGW action 返回 Windows 路径。preset 使用 FILEPATH 声明编译器、
归档器和 windres，CMake 因此在生成资源编译器配置前规范化路径。SaltsBase 的
C11 扫描头显式引入 stdatomic.h，使 MSVC ARM64 编译 SIMDe 内的原子栅栏。
两项问题在 [SaltsBase 首轮 CI](https://github.com/qigao/salts-base/actions/runs/44694282141)
复现，修复后 Windows ARM64 的两种配置都进入完整构建。
MSVC ARM64 随后在 Disruptor 链接时报告 x86 的 _mm_pause 未定义。Windows
路径改用 Windows SDK 的 YieldProcessor()，由 SDK 为目标架构选择处理器提示。
[SaltsBase 链接日志](https://github.com/qigao/salts-base/actions/runs/44695017585/job/136150104644)

Windows SDK 的 rpcndr.h 将 small 定义为 char。SaltsBase 的 JSON SAX 数字临时
缓冲区、CHttp 测试和 TurboDB MySQL 解码中的同名局部变量改为描述其用途的名称。
9 项相关本地 CTest 通过。
[SaltsBase 编译日志](https://github.com/qigao/salts-base/actions/runs/44696074550/job/136153577769)

两种 Android ABI 都需要宿主机版 SaltsBase 和 SaltsUtils。Linux GCC
arm64 使用独立的 SaltsCNet GCC 包，Clang arm64 使用 `linux-arm64` 包。
`.env.example` 选择同一组发布，切换平台需重新恢复匹配的依赖并执行
`cmake --fresh --preset <preset>`。

MSVC 19.52.37029 的 ARM64 代码生成会在结构体字段赋值后写入重叠的零值。
反汇编中，向偏移 0x38 写入生命周期 size 后，偏移 0x20 处的 32 字节清零
覆盖了该字段。Actor 的 Action binding 也有同样的写入次序。
[CMeta 诊断](https://github.com/qigao/cmeta/actions/runs/44701573552)

受影响的 MSVC ARM64 Release preset 使用 `/Od /DNDEBUG`。
`/O1` 和 `/Qvec-` 的验证仍有失败，`/Od` 通过 CMeta 的 120 项 CTest。
恢复优化的验收条件是修正的工具链通过相同的原生和下游测试。性能影响未测量。
[CMeta ARM64 验证](https://github.com/qigao/cmeta/actions/runs/44703550580)

macOS ARM64 首轮构建完成后，6 项 ORM、Server 和 Control 的 CTest 在加载
`libturbo_orm.dylib` 时找不到 `@rpath/libdata_bind.dylib`。补齐 SaltsUtils 后，
`test_flowie_orm_runtime` 仍无法加载 PostgreSQL 驱动。发布包的 Mach-O 加载命令
显示 PostgreSQL 和 MySQL 模块还依赖 SaltsCNet 的 `libcnet.dylib` 和
`libsalts_crypto.dylib`。Flowie 将这两个 SDK 的库目录加入 macOS 构建 RPATH。
归档保留独立 SDK 的安装布局。
[Flowie 首轮 macOS CTest](https://github.com/qigao/flowie/actions/runs/44707944027/job/136192209383)
[Flowie ORM 驱动 CTest](https://github.com/qigao/flowie/actions/runs/44708848545/job/136195221247)

## 2026-11-21：Paho 外部互操作和 Windows CTest 依赖

已使用 [Paho `fdab9d2b`](https://github.com/eclipse-paho/paho.mqtt.testing/tree/fdab9d2b1ce125385135e5d9f282a0ad315268cc/interoperability)
连接本机临时 Flowie TCP listener。使用 loopback 和内存 SQLite，因为 Paho 启动清理会订阅
`#` 并删除收到的 retained 消息。`client_test5.py -p <port> Test.test_basic
Test.test_shared_subscriptions` 两项通过。MQTT 3.1.1 的用例名为 `Test.testBasic`。
其脚本解析 `-p` 后仍把它交给 unittest。复验可在加载脚本时让 unittest 只接收用例名。
该用例通过，Windows 客户端收包线程在关闭时打印了 `OSError 10038`。

未加载本轮 vcpkg triplet 的 `bin` 时，Windows 的 transport baseline 与 protocol repository
CTest 返回 `0xc0000135`。将同一 `CMakeCache.txt` 对应的 vcpkg triplet、各 SDK 和 LLVM-MinGW
运行目录加入当前进程 `PATH` 后，六项原生 CTest 通过，覆盖协议矩阵、corpus、传输、
repository 和两项 HTTPS 安全测试。

## 2026-11-21：外部 MQTT 套件与健壮性复验

Windows LLVM-MinGW Release 使用独立 loopback Broker。Paho `fdab9d2b` 运行 37 项，27 通过、10 失败。
其中 MQTT 5 零长度 Client ID 用例捕获了断言异常并返回布尔值，unittest 仍计为通过。日志复核为 26 项无断言失败、10 项明确失败、1 项吞掉断言。
`mqtt5-conformance` `1412692e` 在 TCP 与 WS 上运行 194 项，173 通过、17 失败、4 跳过。
Mosquitto `v2.0.22` 选取 24 项，校准测试模板的成功 CONNACK 能力属性后 17 通过、7 失败。
原始结果、逐项日志、启动器和分析存放在本地 `build/external-mqtt-20261121/`。

- HIGH Keep Alive=2 秒，静默 5 秒仍可收到 PINGRESP。连接协商计算了超时但未应用。
- HIGH 发送窗口用满时，队首 QoS PUBLISH 阻塞控制回复。释放首个 QoS 1 确认后，排队消息和 PINGRESP 才到达。
- HIGH 单条消息超过订阅端协商的 Maximum Packet Size 会关闭订阅者，导致后续合规消息无法到达。
- HIGH 文件 SQLite 下观察到 retained 的 PUBACK 早于落库。ACK 后立即终止时消息丢失，等待 SQLite 行提交后再终止则恢复成功。
- HIGH 无安全 provider 的监听器接受了带不支持 Authentication Method 的 CONNECT。此结果来自匿名测试配置，受保护配置尚待单独复验。

启用 `topic_alias_maximum: 10` 后，6 项 MQTT 5 alias 一致性复验均通过。Mosquitto 多主题 retained
脚本依赖自己的投递次序，按主题集合独立复验，删除结果正确。复验入口见本地证据目录的 `analysis.md`。
本轮结论限于 Windows loopback、TCP/WS 和 SQLite。TLS/WSS、Control ACL、压测、长稳和集群待验证。

### 五项 HIGH 修复复验

- MQTT Keep Alive 的计时由连接 owner 管理。在成功 CONNACK 后启动，每个完整入站 MQTT
  报文刷新计时。Keep Alive 为 2 秒的测试在最后一次有效报文后约 3.0 秒断开。
- 发送窗口满时由连接队列放行最早可发送的控制报文。独立 QoS 1/2 测试在确认首个
  PUBLISH 前收到 PINGRESP 和 SUBACK。QoS 2 的 PUBREL 可先行，PUBCOMP 后继续按序投递。
- 包长限制在保留 Packet ID 和提交会话状态前检查。离线重放删除已提交的超长 PUBLISH 时，
  仓库 CAS 需要推进会话修订号。顺序删除并更新移动条目的 Packet ID 索引，可保持其他消息的顺序。
  在线投递、retained 重放、离线重放及会话投递配额已满的复验均通过。
- 单机 SQLite 的 retained 写入先于默认 QoS 1 PUBACK 或 QoS 2 PUBREC。收到对应 ACK
  后立即终止进程并重启，各通过 3 次。触发 SQLite 拒绝写入时，发布者连接关闭且未收到 PUBACK。
- 匿名 listener 对不支持的 Authentication Method 返回 CONNACK `0x8c` 并关闭连接。
  随后的普通匿名连接可以正常收发。

使用 Windows LLVM-MinGW Release 的 `native-release-user` preset 完整构建。CTest 75 项通过，
`test_flowie_mqtt_client_live` 在现有配置中禁用。`mqtt5-conformance` 为 179 通过、11 失败、4 跳过，
原有失败中 6 项转为通过。Paho 为 30 项报告通过、7 项失败，其中 1 项报告通过仍吞掉断言。
Mosquitto 校准组仍为 17 通过、7 失败，失败项与原基线一致。

复验入口为 `python -B build/external-mqtt-20261121/check_fixes.py`，每次写入独立的
`fix-verification-*` 目录。此轮最终结果位于 `fix-verification-pshdap3s/results.json`。
CTest、一致性、Paho 和 Mosquitto 日志分别位于
`ctest-final.log`、`conformance-final/`、`paho-after-fixes/` 和 `mosquitto-after-fixes/`。

### MQTT 5 一致性剩余 11 项的归因

- **HIGH，共享订阅离线投递，4 项同源。** 四项用例在读取离线发布的 QoS 1 marker 前已经超时，
  因此尚未执行 DUP、RETAIN 或探测消息 QoS 的断言。共享 fanout 只选有活动 route 的成员，
  跳过离线持久会话。普通持久订阅可以重放，在线共享订阅可以投递，共享会话重连也返回
  Session Present=1，但没有收到离线消息。先确定全员离线时 QoS 1/2 的排队归属和单次分配规则，
  再独立验证首次投递标志及授予的 QoS。QoS 0 离线存储是可选的。
- **MED，Subscription Identifier 方向校验，1 项。** 现有日志中服务端未关闭连接。
  通用 PUBLISH 属性解析允许该属性，因为服务端出站报文需要它。服务端入站路径缺少方向校验，
  fanout 阶段才剥离该属性。校验归属为服务端入站 PUBLISH，放在 alias 更新、会话变更和应用接纳之前。
- **LOW，Topic Alias 配置，3 项。** 默认 alias 上限为 0，套件的三个正向用例仍主动使用 alias。
  本次以 `topic_alias_maximum: 10` 重跑，6 项 alias 用例全过。正向套件应启用并核对协商上限。
- **LOW，错误 CONNACK 断言，3 项。** 三者都用未知协议级别 99 并要求收到错误 CONNACK。
  MQTT 5 第 3.1.2.2 和 3.2.2.2 节 允许服务器直接关闭。套件中对应的未知版本测试已通过。
  错误 CONNACK 的 Session Present、Reason Code 和关闭行为宜使用实际返回错误 CONNACK 的场景验证。

同一 Windows Release broker 的 alias 复验见 `alias-enabled-review/test.log`，共享订阅对照见
`shared-analysis-t_obtor7/results.json`。两者均位于 `build/external-mqtt-20261121/`。
规范依据见 [MQTT 5.0 OASIS 标准](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html)。

### 剩余用例修复与复验

- 单机共享订阅优先投递给在线成员。全员离线时，从轮询候选中选一条有剩余配额且未过期的
  持久会话，复用会话的有界投递和重放。四项共享订阅 conformance 通过，覆盖首次重放的
  QoS、DUP 和 RETAIN。双成员文件 SQLite 复验覆盖进程重启，每条消息只恢复到选中的成员，
  两条消息分别进入不同会话，且在线成员能优先接收新消息。
- SQLite 重启与同进程重连的 DUP 不同。现有仓库快照把 QUEUED 保存成等待确认状态，
  所以重启后的恢复会带 DUP=1，即使宕机前未送出。该行为也适用于普通持久订阅。
  复验观测见 `shared-delivery-c1u_9c1r/results.json`。
- 共享的服务端 PUBLISH 入站校验在处理别名或接纳消息前拒绝 Subscription Identifier，
  将 DISCONNECT `0x82` 交给终止回复队列。TinyTest 覆盖原因码和关闭。
- Windows Release 完整构建通过，CTest 75 项通过、1 项按现有配置禁用。
  启用 alias 10 后，原版 mqtt-lib `1412692e` 为
  187 通过、3 失败、4 跳过。应用 [错误 CONNACK 测试补丁](interop/mqtt5-conformance/error-connack.patch)
  后为 190 通过、0 失败、4 跳过。结果分别位于 `conformance-configured-stock-final/` 和
  `conformance-configured-error-connack/`。两次均覆盖 TCP 和 WS。

本机复验脚本为 `build/external-mqtt-20261121/run_configured_conformance.py`
和 `build/external-mqtt-20261121/check_shared_delivery.py`，CTest 日志为同目录下的
`ctest-after-conformance.log`。

### Paho、Mosquitto、SQLite 和传输安全复验

- Paho 37 项复跑，29 项通过且无断言失败、7 项失败、1 项吞掉断言。Mosquitto 校准组
  24 项复跑，17 项通过、7 项失败。两套与上一轮的失败集合完全相同。
  Paho `test_flow_control2` 仍在等待错误反馈时观察到连接中止。Mosquitto 的 MQTT 3.1
  QoS 2 重连、Will Delay 重连与会话接管仍有失败。逐项输出位于 `paho-current-regression/`
  和 `mosquitto-current-regression/`。
- 文件 SQLite 复验覆盖普通和共享订阅的过期离线消息、未过期消息重放、确认后再次重启、
  retained 删除与写入失败。过期消息和已确认消息均未重放，retained 删除在重启后仍有效。
  两种订阅的未过期消息首次在重启后重放时均观察到 DUP=1。证据为
  `sqlite-restart-matrix-cyj27txo/results.json`、`fix-verification-7cxex858/results.json`
  和 `shared-delivery-q1u6bd5e/results.json`。
- TLS 与 WSS 的可信证书连接均收到 CONNACK 和 PINGRESP。未知 CA、SAN 不匹配和过期
  证书在客户端验证阶段均拒绝，合计 8 个场景。用于该 fixture 的 RSA 私钥以未加密 PKCS#8
  写出，匹配当前 GmSSL PEM 读取入口。证据为 `tls-matrix-jqgfbev0/results.json`。
  CTest `mqtt-security|security-contract` 30 项通过，见 `ctest-security-current.log`。
- Windows LLVM-MinGW 目标拒绝 `-fsanitize=fuzzer`，持续 fuzz 构建止于编译，见
  `sanitizer-configure-build.log`。独立的 ASan/UBSan 构建运行固定协议、矩阵和 corpus。
  严格 UBSan 首次在 corpus xorshift32 的有意截断处终止。将左移放到 64 位，并显式截断为
  32 位以保留原字节序列后，严格 ASan/UBSan 三项均通过，Release corpus 也通过。
  日志为 `asan-ubsan-corpus-runtime.log` 与 `asan-ubsan-corpus-final.log`。

本轮日志均在 `build/external-mqtt-20261121/`。仍待验证 MQTT 入站 mTLS、组权限叠加个人权限
的 MQTT 端到端场景、固定 Broker 双向矩阵、Linux Release 和长稳。当前环境没有可用的 WSL
发行版或 Docker CLI，部署级 PostgreSQL 和多节点验证需要隔离环境。

### Paho 和 Mosquitto 失败归因

- **HIGH，Will 丢失。** Mosquitto `07-will-takeover` 的 MQTT 3.1.1 同 ID 接管失败。
  独立复验中普通异常断开发布 Will，接管关闭旧连接后订阅者仅收到新连接的标记。
  MQTT 5 补测也确认，带未到期 Will 的会话被 Clean Start 替换时旧 Will 丢失，恢复同一
  会话则正确抑制 Will。`flowie_session_owner_open_impl` 替换时先清除旧 Will，接管路径
  也跳过旧连接的关闭处理。修复需在会话替换前保留旧 Will，按版本及会话是否结束触发一次。
  用 `review_will_cases.py` 复跑。结果位于 `will-review-s12yvy5c/results.json`。
- **MED，MQTT 3.1 重传 PUBREL。** Mosquitto `03-publish-c2b-disconnect-qos2` 和分版本复验
  均在 MQTT 3.1 带 DUP 的重传上收到 EOF。固定头只接受 PUBREL 标志 `0x02`。
  [IBM MQTT 3.1 规范](https://public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html#dup-flag)
  支持此处的 DUP。修复仅适用于 level 3，保留 3.1.1/5 的固定标志校验。
  同一复验的 3.1.1 返回 PUBCOMP，MQTT 5 返回 PUBCOMP `0x92`。
- **MED，Topic Alias=0 错误反馈。** Paho `test_client_topic_alias` 收不到 DISCONNECT 回调。
  通用属性解析提前返回错误，endpoint 的 alias `0x94` 路径无法处理 0 值。MQTT 5 第 3.3.4 节
  指定原因码 `0x94`。修复需保留零值拒绝，并将具体原因传到终止回复。
- **MED，Receive Maximum 反馈待定位。** Paho `test_flow_control2` 在读取回复时收到
  WinError 10053。endpoint 有排队 `0x93` 的分支，现有客户端日志无法区分该分支未执行
  与终止回复未送达。需核对 broker 终止回复队列和网络关闭顺序。
- **测试条件。** 两个 Mosquitto Will Delay 用例都省略 Session Expiry，实际默认为 0。
  当前 broker 的独立复验分别测得默认 expiry 约 0 秒、expiry 60 时约 2.02 秒。
  测试延迟需设置非零 expiry。测试恢复原会话时用 Clean Start=0。
  Paho MQTT 3.1.1 和 5 的两个订阅拒绝用例需要 ACL 拒绝夹具，当前用的是匿名监听器。
  Mosquitto 正向别名用例需要非零入站 alias 上限。配置 alias=10 的一致性测试已通过。
- **套件预期。** Paho 的 MQTT 3.1.1 空 Client ID、MQTT 5 出站 Topic Alias 与
  Server Keep Alive 覆盖都要求可选能力。MQTT 5 空 Client ID 且 Clean Start=0 的
  测试吞掉了断言，Flowie 的拒绝策略属于规范允许的行为。Mosquitto 无匹配订阅者时
  要求 `0x10`，规范也允许成功 `0x00`。跨主题 retained 用例固定顺序，按主题集合
  独立复验的四次交付数量为 3、2、1、0，删除正常。

源日志为 `paho-current-regression/`、`mosquitto-current-regression/`，QoS 2 和 retained 对照
为 `interop-review-pf638at3/results.json`，均在 `build/external-mqtt-20261121/`。
规则参见 [MQTT 3.1.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)
和 [MQTT 5.0](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html)。

### Will、PUBREL 和终止回复修复

- 先验证新连接，再用旧会话的 pending-Will 记录完成该发的 Will。旧连接进入关闭，待发 Will
  交给应用后再复用或删除旧会话。到期任务与替换使用同一会话内标志协调发布。
  TCP 复验中 MQTT 3.1.1 接管收到旧 Will，MQTT 5 Clean Start 替换收到旧 Will，
  同一 MQTT 5 会话在 Will Delay 内恢复时仍正确抑制。见 `will-interop-final-verified.log`。
- MQTT 3.1 接受合法的重传 PUBREL DUP 位，3.1.1/5 继续拒绝这个位。协议 TinyTest 覆盖三版。
  Mosquitto QoS 2 案例在 3.1/3.1.1 通过后进入 MQTT 5，最后一次重传收到规范允许的 `0x92`。
  将该处的期望校准为 `0x92` 后，三版均通过。
- Topic Alias=0 由通用 parser 拒绝，服务端解析失败后识别该属性并发送 `0x94`。
  Paho `test_client_topic_alias` 已通过，正式 TCP 测试检查具体原因码和连接关闭。
- TCP/TLS 以 CNet final write 提交终止报文，先完成已接纳的写请求，再关闭连接。
  Receive Maximum 用例的正式 TCP 测试检查受理范围内的 PUBREC 在 `0x93` 之前。
  Paho 只读变体使用相同超限输入，收到 64 个 PUBREC 及 `0x93`。原版 Paho 在读取
  PUBREC 时继续自动发送 PUBREL，Windows 仍观察到 WinError 10053。分别见
  `paho-quota-readonly-uvpbkm05/test.log` 和 `paho-quota-wire-22zivj1b/test.log`。

校准后的四个 Mosquitto QoS 2 / Will 场景通过，日志位于 `mosquitto-adapted-interop-tm9klw7l/`。
成功条件包括显式 Session Expiry，恢复原会话时 Clean Start=0，零 Will Delay 的接管会发布旧 Will，
以及最后一次 MQTT 5 PUBREL 允许 `0x92`。
Windows Release 完整 CTest 75 项通过、1 项禁用。适配后的 MQTT 5 TCP/WS 一致性测试
190 项通过、4 项跳过。日志分别为 `ctest-interop-final-verified.log` 和
`conformance-terminal-will-final/test.log`。证据均在 `build/external-mqtt-20261121/`。

### 剩余 MQTT 问题分诊

以下实测使用 Windows `native-release-user` 构建和 loopback。测试脚本及结果在本地
`build/external-mqtt-20261121/`。本轮只更新交接记录。

- **HIGH，broker 进程访问冲突，归因待定。** 两次 HTTPS Auth/ACL 初步复验中，客户端
  观察到 WinError 10054，broker 子进程记录 `0xC0000005`（`STATUS_ACCESS_VIOLATION`）。
  一次发生在同连接读下一条订阅应答时，另一次发生在后续连接读 CONNACK 时。每个场景
  独立启动 broker 并正常结束 MQTT 会话的 8 个对照均通过。现有对照同时改变了进程复用
  与连接收尾方式，缺少故障栈，尚不能归因于 ACL 拒绝、会话清理或 SDK。先收集带符号
  的异常栈，确认 faulting module 和线程，再确定归属。原始退出记录为
  `suback-triage-6b2zjlq6/broker-exit.json` 和 `suback-triage-6hmi9xgb/broker-exit.json`。
  [Windows NTSTATUS 定义](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/596a1078-e883-4972-9bbc-49e60bebca55)。
- **MED，SQLite 首次投递错误设置 DUP，已确认。** 普通订阅、共享订阅各测 QoS 1/2，
  4 个重启对照在尚未发出 PUBLISH 时持久化为 `WAIT_ACK`（2）或 `WAIT_PUBREC`（3），
  存储报文 DUP 位为 0。重启后的第一次投递全部变为 DUP=1。同进程的 4 个对照首次
  均为 DUP=0，未确认后再重连时均用相同 Packet ID 且 DUP=1。
  `flowie_session_owner_repository_snapshot` 将 `QUEUED` 投影为等待确认状态，恢复后
  `flowie_session_owner_delivery_pending_at_ex` 按该状态置 DUP。旧记录序列化存在同样投影。
  修复需保留从未尝试发送的持久化状态，并明确在首次发送交接处如何记录状态、处理未接纳和
  崩溃窗口。已有等待确认记录含义不确定，需保留兼容语义。结果为
  `dup-triage-1ua3jlka/results.json`，复验入口 `triage_sqlite_dup.py`。其他存储后端尚未复验。
  [MQTT 5 §3.3.1.1](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html)。
- **MED，MQTT 5 Receive Maximum 关闭结果随 Windows TCP 双向收发变化。** 同一 Paho
  `test_flow_control2` 输入下，将读取期间回发的 PUBREL 上限设为 0、1、8 时，6/6 收到
  64 个 PUBREC，随后收到 DISCONNECT `0x93`。持续回发时 2/2 在读到 32 或 43 个 PUBREC
  后遇到 WinError 10053。已验证 MQTT 协议分支能发出正确原因码。双向场景还需核对
  `cnet_send_buffer_and_close` 的最终写完成事件和 TCP 关闭顺序。10053 本身无法确定 TCP
  FIN/RST 的发出方。结果为 `paho-quota-triage-cztx76bb/results.json`，复验入口
  `triage_paho_quota.py`。[Winsock 错误码](https://learn.microsoft.com/en-us/windows/win32/winsock/windows-sockets-error-codes-2)。
- **LOW，Paho 订阅拒绝需调整夹具和断言。** 通过独立的本地 HTTPS Auth/ACL 夹具，MQTT
  3.1.1 和 5 的 8 个正常场景均通过，包括单个允许、单个拒绝及两种混合顺序。拒绝码
  分别为 `0x80` 和 `0x87`，混合回复顺序对应订阅顺序，随后均能收到 PINGRESP。
  Paho 原用例连接匿名监听器，MQTT 5 还固定断言 `0x80`。移植时配置真实拒绝并按版本
  断言原因码。记录为 `suback-triage-bmergtcd/results.json`，复验入口 `triage_suback.py`，
  5 个相关正式 CTest 通过，见 `ctest-acl-triage.log`。夹具验证 HTTPS provider 到 MQTT
  订阅回复，群组与个人 ACL 的 Control Repository 到 broker 联动还需端到端覆盖。
  [MQTT 5 §3.9.3](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html)。

其他上游差异已归入前文的套件预期和测试条件。MQTT 5 一致性测试的 4 项跳过需要
`$SYS` 发布权限或系统主题发布能力（2 项）、严格 Client ID 字符集模式（2 项），
见 `conformance-terminal-will-final/test.log`。

### 接管提交、TCP 终止写和 QoS 重放回归

- `test_flowie_protocol_repository` 用 SQLite trigger 拒绝替换会话的写入。失败时旧连接仍回答
  PINGREQ 且没有发布 Will。移除 trigger 后同一 Client ID 接管成功，旧连接关闭并发布一次 Will。
  替换事务提交后，旧 Will 由内存中的会话过期任务派发。仓库结构未迁移，提交后至派发前的
  进程崩溃可能丢失这份旧 Will。
- 在 Windows loopback 暂停客户端读仍可能很快完成 CNet 写入，无法稳定制造背压。
  `test_flowie_connection` 延迟处理前一笔写入完成回调，验证终止写完成后才开始 TCP EOF
  等待，并核对两笔报文的顺序。
- `test_flowie_transport_baseline` 覆盖 Receive Maximum 缩小后的重连。旧 PUBREL 能越过
  窗口阻塞，其 PUBCOMP 不释放新连接的 PUBLISH 配额，未尝试发送的在线积压首次投递使用
  DUP=0。Windows `native-release-user` 完整构建和 CTest 通过（75 通过，1 项公网测试禁用）。
  补跑本地 `review_will_cases.py`，验证 MQTT 5 Will Delay、恢复与替换，以及 MQTT 3.1.1
  异常断线和接管。结果位于 `build/external-mqtt-20261121/will-review-1dt_ul3w/results.json`。

### 旧 Will 恢复与 `$SYS` 复验

- 独立协议仓库在同一事务保存替换会话与 `replaced_wills` 行。SQLite 回归在旧 Will 入表失败时
  检查整个接管回滚，随后阻塞应用派发并重建 endpoint，验证从仓库恢复并派发旧 Will。应用接受后
  删除行，接受与删除之间崩溃可能重派发。自建仓库升级和回滚见 [V2 仓库说明](docs/ADR_TYPED_ORM_PERSISTENCE_V2.md#pending-will-after-session-replacement)。
- `$SYS/broker/version` 是 standalone 的只读 retained QoS 0 主题。正式测试覆盖显式匹配、根通配符
  隔离和 MQTT 5 写入拒绝。启用两个 `$SYS` 能力后，适配的 MQTT 5 TCP/WS 套件为 192 通过、
  0 失败、2 跳过。跳过项对应可选的严格 Client ID 字符集。日志为
  `build/external-mqtt-20261121/conformance-system-s448vwdf/test.log`。
- 原版 Paho `test_flow_control2` 复跑通过，见 `paho-stock-quota-im03m93y/test.log`。最新 SQLite
  普通及共享订阅的 QoS 1/2 冷启动首次投递为 DUP=0，未确认后的重放仍为 DUP=1，见
  `dup-triage-os7_ogwc/results.json`。两处证据均在 `build/external-mqtt-20261121/`。
- `test_flowie_transport_baseline` 新增入站 TLS 客户端证书三组对照。无证书及不可信证书均未进入
  MQTT Auth，有效证书提供已验证指纹并收到 CONNACK 和 PINGRESP。`native-release-user`
  完整构建和 CTest 为 75 通过、1 项公网测试按配置禁用。
- HTTPS Auth/ACL 的正常互操作复跑仍记录 Windows `0xC0000005`，发生在读取 CONNACK 时。
  `suback-triage-xy4g9qlk/v4-allow/broker-exit.json` 只有退出状态，Windows 事件日志和用户转储
  目录未找到对应故障栈。当前状态仍为待归因。

### HTTPS Auth 访问冲突已定位到 DLL 调用栈

- **HIGH，事实。** 以 GDB 运行现有正常 HTTPS Auth/ACL 夹具。一次 MQTT 3.1.1 连接和订阅完成，
  随后的连接在 HTTPS Auth 开始建立出站连接时收到 `SIGSEGV`。完整调用栈见
  `build/external-mqtt-20261121/suback-triage-sgokcfhs/v4-deny/broker.log`。路径为
  `flowie_server_http_authenticate` → `chttp_post` → `cnet_buffer_acquire` → `mem_get_buffer`。
- `cnet_buffer_acquire` 使用 `mem_global()` 为 CNet 连接分配接收缓冲区。发布版 `libsalts.dll`
  的故障指令正在读取内部缓存槽取出的缓冲区的 `capacity`。构建目录实际加载的 DLL 与本地所选
  SaltsBase `0.1.8` SDK 的 SHA-256 一致。涉及 Chttp `0.1.2` 与 SaltsCNet `0.1.16`。
- 缓存项何时失效仍待确认。当前证据定位了失败的读取和调用者，尚无法区分缓存槽生命周期问题与更早的
  内存破坏。未改动 Flowie 或 SDK 源码。Windows `native-release-user` 的 `test_flowie_transport_baseline`、
  `test_flowie_server_http_security`、`test_flowie_control_https_integration` 均通过。

### 未注册 Core 测试恢复与 scheduled soak

- `test_flowie.c`、`test_flowie_ingress.c`、`test_flowie_mqtt_endurance.c` 原先存在但未注册，且仍引用
  已移除的 TurboFlow API。迁移后分别覆盖当前 Endpoint/Session、同 lane Ingress，以及持久会话、
  共享订阅和关闭收敛。共享路由用例必须以 `flowie_test_cnet_readable(..., 1u)` 推进两个 subscriber；
  传入 `0u` 只检查已有事件，不会驱动 CNet，表现为稳定超时而不是 broker 未入队。
- Windows CTest 直接启动时，间接 SDK DLL 不会自动出现在测试进程搜索路径。此次验证只在当前
  PowerShell 进程的 `PATH` 前置同一构建的 `bin`、CHttp/TurboDB/Salts Utils SDK `bin` 和当前
  vcpkg triplet `bin`，未复制或提交 DLL。缺失时进程退出码为 `0xc0000135`。
- scheduled soak 的旧过滤器指向已删除的测试。现已改为当前 Endurance、Ingress、TLS/WSS 和
  TurboRaft 用例。Windows 的首次 `process_spawn` 会延迟初始化常驻句柄，采样前需预热；短暂 worker
  线程波动不能当作泄漏，销毁后使用有界 settle barrier 验证句柄和线程回落。
- `native-release-user` 完整构建通过。soak 随正式 `BUILD_TESTS` 测试图注册，共 80 个 CTest，
  实际执行 79 个全部通过，`test_flowie_mqtt_client_live` 仍按配置禁用；总耗时 103.21 秒。

### 公网 MQTT live 复验

- 启用 `FLOWIE_MQTT_PUBLIC_LIVE_TESTS` 后，正式夹具覆盖 EMQX TCP 1883、TLS 8883、WS 8083、
  WSS 8084 和 TLS request-response 共 5 个场景。使用修复系统根准入的临时 Salts Core
  `salts_crypto.dll` 时五项全部通过；这确认旧 TLS/WSS 失败来自 GmSSL 在系统根包中过早终止，
  而不是服务器证书链或 Flowie MQTT 处理。
- HiveMQ 公网场景已从正式夹具删除；通用 fixed-broker 互操作入口不变，仍可由可达的受控 Broker
  配置驱动。此前本机 DNS 无法返回 `broker.hivemq.com` 的 A/AAAA 数据，未进入 TCP/TLS。
- CMake 增量构建通过，聚焦公网 CTest 的 5/5 个 TinyTest 场景通过，耗时 41.40 秒。验证后
  使用 SHA-256 核对并恢复 Flowie 原 `salts_crypto.dll`；临时备份已删除。

### MQTT Client 本地契约回归

- 新增正式 CTest `test_flowie_mqtt_client_contracts`，使用真实 loopback TCP/TLS 夹具覆盖 13 个场景：
  命令槽位和 owned-byte 满载时的批量 PUBLISH 原子拒绝、MQTT 5 增强认证及在线重认证、认证方法
  不匹配和回调拒绝、mTLS 客户端身份成功与缺失、SUBSCRIBE/UNSUBSCRIBE 输入所有权与 handler
  顺序、入站 QoS 2 所有权上限、配置准入和版本锁定。测试不依赖公网，也未增加生产测试钩子。
- `native-release-user` 完整 build graph 构建通过；该 CTest 连续运行 20 次全部通过。相邻的
  Workbench、原 Client 和 close-full 目标 4/4 通过。当前 CMake 图为 81 个 CTest；排除会访问
  公网的 `test_flowie_mqtt_client_live` 后，本地完整图 80/80 通过，总耗时 139.34 秒。
- 2026-11-22 再次以修复版 `salts_crypto.dll` 运行公网 live，五个场景均在 CONNECT 阶段返回
  `SALTS_ETIMEDOUT`（`-4050`）。同一进程随后用 `Test-NetConnection` 检查 EMQX 的 1883、8883、
  8083 和 8084，`broker.emqx.io` 均无法完成 DNS 解析；TCP/TLS/WS/WSS 尚未开始，因此该结果
  不能用于判断证书策略。修复版 DLL 自身的 `saltscore.gmssl_tls_io_abi` 正式 CTest 通过；Flowie
  原 DLL 已恢复并以 SHA-256 `094F782BDCBAEDCC9CF4AFDA9387B991FFCFADB804EC697DE9466220465BC3FA`
  核对，临时备份已删除。
- 2026-11-22 22:06–22:07（北京时间）用 `Resolve-DnsName -DnsOnly -NoHostsFile -QuickTimeout`
  对照本机默认 DNS 与 `-Server 8.8.8.8`、`223.5.5.5`、`119.29.29.29`。默认 DNS 及直接
  查询路由器和 WLAN 的 IPv6 DNS 时，两个 broker 的 A 查询均超时。三家公共 DNS 均返回
  IPv4 记录。EMQX 的 CNAME `prod-blue.public-broker.com` 在本机可查到，但其 A 查询同样
  超时；`example.com` 的 A 查询成功。复验时按域名查询并记录当时的 TTL，保留原始主机名
  用于 TLS 身份校验。该检查定位了本机 DNS 路径的差异，尚未验证 MQTT 端口或握手。
- 随后用阿里 DNS 当次返回的两个 IPv4 地址直连 EMQX。独立探针在两个地址的 1883、8883、
  8083 和 8084 均收到成功 CONNACK 和 PINGRESP（8/8）。8883 和 8084 使用 EMQX 发布的 CA
  验证 `broker.emqx.io`，均协商 TLS 1.3。8083 和 8084 均完成 `/mqtt` 升级并协商 `mqtt`
  子协议。Windows 拒绝非管理员终端添加域名级 NRPT 规则，系统默认解析仍超时，正式公网
  CTest 待默认 DNS 可用后复验。
- 2026-11-22 22:16（北京时间）复查时，Windows 默认 DNS 和 `223.5.5.5` 均返回两个相同的
  EMQX IPv4 地址。以临时修复版 `salts_crypto.dll` 执行
  `ctest --preset native-release-user -V -R '^test_flowie_mqtt_client_live$'`，正式 CTest 1/1
  通过，TCP、TLS、WS、WSS 和 MQTT 5 请求响应共 5/5 场景、11 条断言通过，用时 40.25 秒。
  测试沿用系统信任路径。原 DLL 已按上述 SHA-256 核对并恢复，临时备份已删除。
- 公网夹具新增 MQTT 5 TCP、MQTT 3.1.1 TLS，以及 WSS 上的 MQTT 5 Topic Alias 复用和
  Subscription Identifier 回传。使用同一临时修复版 DLL 时，首轮新增场景 3/3 通过，整组
  7/8 通过。原有 MQTT 3.1.1 TCP 场景在 CONNECT 阶段返回 `SALTS_ECONNRESET`（`-4010`）。
  同一命令复跑 8/8、18 条断言全部通过，用时 66.87 秒，连接重置未复现。恢复原 DLL 并核对
  SHA-256 后，原 Client、Contract 和 close-full 本地 CTest 3/3 通过。
- 2026-11-23 参考 Paho `client_test5.py` 补充公网 User Properties 与 No Local。TLS 路径验证
  QoS 0/1/2 的同名属性值和转发顺序。TCP 路径等待两个独立客户端均收到 SUBACK，再验证
  双向 QoS 1 投递和一秒内没有自收。两个夹具通过 `try_destroy` 检查排空结果，未排空时保留
  回调状态。最终使用临时修复版 DLL 执行公网 CTest，10/10 场景、28 条断言通过，耗时
  48.73 秒。原 DLL 已恢复并核对 SHA-256，临时备份已删除。
- Windows LLVM-MinGW 的 ASan+UBSan 独立构建中，原 Client、Contract 和 close-full 三个正式 CTest
  3/3 通过，Contract 另连续运行 10 次全部通过。临时 user preset 已移除。仓库 TSan preset 仅在
  Linux 可见，当前机器没有注册 WSL 发行版，因此未执行 TSan；不能把 ASan+UBSan 结果表述为
  数据竞争检查通过。

## 2026-11-23 Slate 本地构建排障

- VS Code 的 Slate 扩展继承旧的 `RE2C_ROOT` 时，`slate graph --tests` 会指向
  salts-utils 已移除的 re2c 4.6.4。SDK 恢复把 `FLOWIE_RE2C_ROOT` 从选定的
  `RE2C_ROOT` 同步写入 `.env`。Slate 专用变量与 CMake 的路径来自同一个恢复结果。
  保留旧进程变量复验，`slate doctor` 从 `.env` 解析到 4.6.7，扩展所用的
  `slate graph --format json --tests` 成功发现目标。
- vcpkg 缺少 GmSSL 的 pkg-config 元数据时，Slate 可找到归档，但测试编译需要
  `GmSSLConfig.cmake` 导出的 `ENABLE_*` 定义。清单补齐定义后，Client 本地 mTLS
  测试通过。Slate 的链接遍历在 shared 目标结束，直接调用协议库的 Core/Cluster
  测试和静态库显式链接 `flowie_protocol`。
- 仅构建指定测试时，Slate 不会执行链接到的静态库的无消费者资源复制任务。
  Control 的本地资源路径改用 `$root/flowie/control`，通过 `compile_commands.json`
  核对展开后的路径。Slate 0.1.14 在此编译定义内保留字面量 `$moduledir`。
- Windows x64 LLVM-MinGW 下，`slate build --preset release` 与
  `slate build --preset cluster` 通过。Release 测试 54/54，`server/cluster` 标签
  23/23。相同 Release 图再次运行 Ninja 时没有编译或链接工作。与已有
  `native-release-user` DLL 比对，Broker 144 个、Client 15 个 PE 导出的名称与序号一致。
