# Flowie

Flowie 是独立的 MQTT server/client 仓库。它拥有 MQTT 协议、Broker Core、会话、订阅、保留消息、认证边界和 Salts 网络生命周期，不依赖任何上层编排产品。

依赖方向是单向的：

```text
下游产品适配器（可选）
          ↓
    Flowie::Flowie
          ↓
Flowie::Protocol + Salts::CNet + CHttp::Client/Server
```

Flowie 的公开 API 不包含上层编排类型。业务系统通过下游 adapter 链接 `Flowie::Flowie` 并完成消息类型映射。

## 构建与测试

Windows 开发构建：

```powershell
cmake --preset win-dev-user
cmake --build --preset win-dev-user
ctest --test-dir build/Msvc --output-on-failure
```

所有构建开关统一声明在 `CMakeOptions.cmake`。常用开关：

- `FLOWIE_BUILD_SERVER`：构建独立 MQTT server，默认开启。
- `FLOWIE_BUILD_TESTS`：构建测试，默认开启。

HTTP 与 WebSocket 能力来自独立的 [qigao/chttp](https://github.com/qigao/chttp) SDK。
配置 Flowie 前将 `HTTP_SERVICES_ROOT` 指向已安装的 profile；默认布局为
`http-services/debug` 或 `http-services/release`，Android 使用
`http-services-android/<profile>`。出站 HTTP 和 WebSocket client 链接
`CHttp::Client`，HTTP 和 WebSocket listener 链接 `CHttp::Server`。Salts 继续提供
CNet、Core、Coroutine 与错误码。

JSON、YAML、命令行和 LTV 解析器由 SaltsUtils SDK 提供，仍使用 `Salts::` 目标名。
`SALTS_UTILS_ROOT` 必须指向匹配的安装 profile（`salts-utils/debug` 或
`salts-utils/release`；Android 为 `salts-utils-android/<profile>`）。这些解析器也用于
Broker 和 server，因此关闭控制面后仍需该依赖。

## 运行独立 Broker

```powershell
flowie_server --host 0.0.0.0 --port 1883 --transport tcp
```

WebSocket listener 可使用 `--transport ws --path /mqtt`。TCP/TLS 由 Salts CNet 承载，WS/WSS 由独立 Chttp SDK 的 WebSocket server 承载；TLS/WSS 证书通过 `SALTS_TLS_CERT_FILE` 和 `SALTS_TLS_KEY_FILE` 配置。

只校验启动参数而不监听：

```powershell
flowie_server --check
```

## CMake 消费

```cmake
find_package(Flowie CONFIG REQUIRED)
target_link_libraries(my_broker PRIVATE Flowie::Flowie)
```

可分别使用 `Flowie::Protocol` 与 `Flowie::Client`。`Flowie::Transport` 是实现细节静态库，不安装、不导出。
