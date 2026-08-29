# Iron rule: every Flowie feature/build switch is declared in this file.
set(CMAKE_COLOR_DIAGNOSTICS ON)

option(FLOWIE_BUILD_TESTS "Build and register Flowie tests" ON)
option(FLOWIE_BUILD_CONTROL "Build the restored Flowie control plane" ON)
option(FLOWIE_BUILD_CLUSTER "Build the TurboRaft-backed Flowie cluster runtime" ON)
option(ENABLE_SANITIZER_ADDRESS "Enable AddressSanitizer" OFF)
option(ENABLE_SANITIZER_UNDEFINED "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_SANITIZER_LEAK "Enable LeakSanitizer" OFF)
option(ENABLE_SANITIZER_THREAD "Enable ThreadSanitizer" OFF)
option(ENABLE_SANITIZER_MEMORY "Enable MemorySanitizer (Clang only)" OFF)

option(FLOWIE_MQTT_PUBLIC_LIVE_TESTS
       "Enable optional Flowie MQTT client external-connectivity smoke tests" OFF)
option(FLOWIE_TURBODB_LIVE_TESTS
       "Enable TurboDB cross-driver live persistence contract tests" OFF)
option(FLOWIE_MQTT_FIXED_INTEROP_TESTS
       "Run Flowie and Mosquitto interoperability against one fixed broker" OFF)
option(FLOWIE_MQTT_FUZZ_TARGETS "Build Flowie MQTT libFuzzer targets" OFF)
option(FLOWIE_MQTT_SOAK_TESTS "Register scheduled Flowie MQTT soak tests" OFF)
option(FLOWIE_MQTT_RELEASE_GATE "Register the strict Flowie MQTT release manifest" OFF)

set(FLOWIE_RELEASE_REVISION "" CACHE STRING
    "Immutable source revision written into Flowie release/nightly evidence")
set(FLOWIE_MQTT_NIGHTLY_SEED "11794061671962178383" CACHE STRING
    "Reproducible Flowie nightly seed")
set(FLOWIE_MQTT_FUZZ_RUNS 100000 CACHE STRING
    "Flowie nightly fuzz input count")
set(FLOWIE_MQTT_NIGHTLY_SOAK_SHORT_MS 1800000 CACHE STRING
    "Short Flowie nightly soak duration")
set(FLOWIE_MQTT_NIGHTLY_SOAK_LONG_MS 3600000 CACHE STRING
    "Long Flowie nightly soak duration")

set(FLOWIE_MQTT_FIXED_BROKER_NAME "Mosquitto-2.0.22" CACHE STRING
    "Fixed broker implementation and exact version")
set(FLOWIE_MQTT_FIXED_HOST "127.0.0.1" CACHE STRING "Fixed broker host")
set(FLOWIE_MQTT_FIXED_TCP_PORT 1883 CACHE STRING "Fixed broker MQTT TCP port")
set(FLOWIE_MQTT_FIXED_TLS_PORT 8883 CACHE STRING "Fixed broker MQTT TLS port")
set(FLOWIE_MQTT_FIXED_WS_PORT 8083 CACHE STRING "Fixed broker MQTT WS port")
set(FLOWIE_MQTT_FIXED_WSS_PORT 8084 CACHE STRING "Fixed broker MQTT WSS port")
set(FLOWIE_MQTT_FIXED_WS_PATH "/mqtt" CACHE STRING "Fixed broker MQTT WS/WSS path")
set(FLOWIE_MQTT_FIXED_CA_FILE "" CACHE FILEPATH
    "CA certificate used to verify the fixed broker TLS/WSS listeners")
option(FLOWIE_MQTT_FIXED_SUPPORT_31 "Fixed broker accepts MQTT 3.1 on TCP and TLS" ON)
option(FLOWIE_MQTT_FIXED_SUPPORT_31_WS
       "Fixed broker accepts MQTT 3.1 over WS and WSS" ON)

# Keep legacy variables synchronized for existing subdirectory CMake code.
set(BUILD_TESTING "${FLOWIE_BUILD_TESTS}" CACHE BOOL "Build tests" FORCE)
set(BUILD_TESTS "${FLOWIE_BUILD_TESTS}" CACHE BOOL "Build tests" FORCE)
set(ENABLE_TESTS "${FLOWIE_BUILD_TESTS}" CACHE BOOL "Build tests" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
