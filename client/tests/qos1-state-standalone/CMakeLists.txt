cmake_minimum_required(VERSION 3.20)
project(FlowieQoS1DeferredStateContract LANGUAGES C)
enable_testing()

# Standalone source contract: no released SDK, vcpkg, network stack,
# Flowie Broker or native-library substitution.
add_executable(test_flowie_mqtt_qos1_deferred_state
  ${CMAKE_CURRENT_LIST_DIR}/../test_flowie_mqtt_qos1_deferred_state.c)
target_include_directories(test_flowie_mqtt_qos1_deferred_state PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../../protocol/include)
set_target_properties(test_flowie_mqtt_qos1_deferred_state PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(test_flowie_mqtt_qos1_deferred_state PRIVATE /W4 /WX)
else()
  target_compile_options(test_flowie_mqtt_qos1_deferred_state PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME flowie.mqtt.qos1_deferred_state
         COMMAND test_flowie_mqtt_qos1_deferred_state)
