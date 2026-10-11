# 固定 Mosquitto 互操作环境

本目录固定使用 `eclipse-mosquitto:2.0.22`，为
`FLOWIE_MQTT_FIXED_INTEROP_TESTS` 提供 TCP、TLS、WS 和 WSS listener。测试不接受匿名降级到
其他 endpoint；TLS/WSS 始终验证本地测试 CA。

先用 GmSSL 或已有 PKI 在仓库外准备一次性证书目录，包含 `ca.pem`、`server.pem`、
`server.key`。CA 必须具备证书签发用途；server identity 必须由该 CA 签发，具备
serverAuth 用途和 SAN `DNS:localhost`、`IP:127.0.0.1`。Mosquitto 使用未加密的 PEM 私钥；
这些材料只用于 loopback 互操作测试。Flowie 的构建和测试不依赖 OpenSSL 命令行工具。

将证书目录挂载到固定 broker：

```powershell
$certDir = Join-Path $env:TEMP "flowie-mqtt-interop-certs"
foreach ($name in @("ca.pem", "server.pem", "server.key")) {
  if (-not (Test-Path -LiteralPath (Join-Path $certDir $name))) { throw "Missing $name" }
}
$env:FLOWIE_MQTT_INTEROP_CERT_DIR = $certDir
docker compose -f flowie/interop/mosquitto-2.0.22/compose.yml up -d --wait
```

配置时必须把同一个 CA 传给固定 broker suite：

```text
-DFLOWIE_MQTT_FIXED_INTEROP_TESTS=ON
-DFLOWIE_MQTT_FIXED_BROKER_NAME=Mosquitto-2.0.22
-DFLOWIE_MQTT_FIXED_CA_FILE=<cert-dir>/ca.pem
-DFLOWIE_MQTT_FIXED_SUPPORT_31=ON
-DFLOWIE_MQTT_FIXED_SUPPORT_31_WS=ON
```

CTest 会分别验证 level 4/5 的 TCP/TLS/WS/WSS 核心 trace、显式声明支持的 level 3
TCP/TLS/WS/WSS trace、MQTT 5 request/response
properties、Topic Alias 与 Subscription Identifier，以及 Mosquitto CLI 到 Flowie 的 retained、
persistent/offline、Will 和 Message Expiry trace。环境或可执行文件缺失会在 configure/gate 阶段
fail fast，不会以 SKIP 或零用例伪装成功。

结束后删除本次 compose project 和测试证书目录：

```powershell
docker compose -f flowie/interop/mosquitto-2.0.22/compose.yml down -v
Remove-Item -Recurse -Force -LiteralPath $env:FLOWIE_MQTT_INTEROP_CERT_DIR
```
