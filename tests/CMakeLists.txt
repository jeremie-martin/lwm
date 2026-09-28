include(FetchContent)
pkg_check_modules(XCB_XTEST REQUIRED IMPORTED_TARGET xcb-xtest)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.0
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2 nlohmann_json)

add_executable(lwm_log_probe log_probe.cpp)
target_link_libraries(lwm_log_probe PRIVATE liblwm)
target_include_directories(lwm_log_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(lwm_logging_tests
  test_logging.cpp
  ${CMAKE_SOURCE_DIR}/src/app/cli.cpp
)
target_link_libraries(lwm_logging_tests PRIVATE liblwm Catch2::Catch2WithMain)
target_include_directories(lwm_logging_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/app)
target_compile_definitions(lwm_logging_tests PRIVATE
  LWM_LOG_PROBE_PATH="$<TARGET_FILE:lwm_log_probe>"
  LWM_BINARY_PATH="$<TARGET_FILE:lwm>"
)
add_dependencies(lwm_logging_tests lwm_log_probe lwm)

add_executable(lwm_tests
  test_config_parser.cpp
  test_events_policy.cpp
  test_invariants.cpp
  test_client_state_policy.cpp
  test_ewmh_classification.cpp
  test_ewmh_policy.cpp
  test_integration_config_reload.cpp
  test_focus_cycling_policy.cpp
  test_focus_policy.cpp
  test_focus_restoration_policy.cpp
  test_integration_client_message.cpp
  test_integration_focus.cpp
  test_integration_focus_input.cpp
  test_integration_lwm_window_class.cpp
  test_integration_notify_attention.cpp
  test_integration_property_notify.cpp
  test_integration_subscribe.cpp
  test_integration_scratchpad.cpp
  test_integration_wm_state.cpp
  test_integration_workspace.cpp
  test_floating_policy.cpp
  test_keybind_policy.cpp
  test_ipc_server.cpp
  test_harness.cpp
  test_layout.cpp
  test_restart.cpp
  test_stacking_policy.cpp
  test_window_rules.cpp
  test_monitor_hotplug.cpp
  test_integration_hotplug.cpp
  test_workspace.cpp
  test_workspace_policy.cpp
  test_integration_logging.cpp
)

target_compile_definitions(lwm_tests PRIVATE
    LWM_BINARY_PATH="$<TARGET_FILE:lwm>"
    LWM_XORG_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/xorg-dummy.conf"
    LWMCTL_BINARY_PATH="$<TARGET_FILE:lwmctl>"
)
add_dependencies(lwm_tests lwm lwmctl)

target_link_libraries(lwm_tests PRIVATE liblwm Catch2::Catch2WithMain PkgConfig::XCB_XTEST nlohmann_json::nlohmann_json)
target_include_directories(lwm_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(Catch)
catch_discover_tests(lwm_logging_tests PROPERTIES SKIP_RETURN_CODE 4 TIMEOUT 60)
catch_discover_tests(lwm_tests PROPERTIES SKIP_RETURN_CODE 4 TIMEOUT 60)
