# Flowie

Flowie 是独立的 MQTT server/client 仓库。它拥有 MQTT 协议、Broker Core、会话、订阅、保留消息、认证边界和 CoroNet 网络生命周期，不依赖任何上层编排产品。

依赖方向是单向的：

```text
下游产品适配器（可选）
          ↓
    Flowie::Flowie
          ↓
Flowie::Protocol + CoroNet
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

## 运行独立 Broker

```powershell
flowie_server --host 0.0.0.0 --port 1883 --transport tcp
```

WebSocket listener 可使用 `--transport ws --path /mqtt`。TLS/WSS 沿用 CoroNet 的证书环境变量 `TURBONET_TLS_CERT_FILE` 和 `TURBONET_TLS_KEY_FILE`。

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
