pkg_check_modules(XCB_XTEST REQUIRED IMPORTED_TARGET xcb-xtest)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG 53d0d913a422d356b23dd927547febdf69ee9081 # Catch2 3.5.0
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03 # nlohmann/json 3.11.3
)
FetchContent_MakeAvailable(Catch2 nlohmann_json)

add_executable(lwm_log_probe log_probe.cpp)
target_link_libraries(lwm_log_probe PRIVATE liblwm)
target_link_options(lwm_log_probe PRIVATE "LINKER:--export-dynamic-symbol=sendmsg")

add_executable(lwm_logging_tests
  test_logging.cpp
  ${PROJECT_SOURCE_DIR}/src/app/cli.cpp
)
target_include_directories(lwm_logging_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_compile_definitions(lwm_logging_tests PRIVATE
  LWM_XORG_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/xorg-dummy.conf"
  LWMCTL_BINARY_PATH="$<TARGET_FILE:lwmctl>"
  LWM_LOG_PROBE_PATH="$<TARGET_FILE:lwm_log_probe>"
  LWM_BINARY_PATH="$<TARGET_FILE:lwm>"
)
add_dependencies(lwm_logging_tests lwm_log_probe lwm lwmctl)

add_executable(lwm_runtime_failure_probe
  runtime_failure_probe.cpp
  ${PROJECT_SOURCE_DIR}/src/app/main.cpp
  ${PROJECT_SOURCE_DIR}/src/app/cli.cpp
)
target_link_libraries(lwm_runtime_failure_probe PRIVATE liblwm)
target_link_options(lwm_runtime_failure_probe PRIVATE "LINKER:--export-dynamic-symbol=xcb_poll_for_event")

# Exercise the real observer with a property write between its request and reply.
add_executable(lwm_observation_probe
  observation_probe.cpp
  ${PROJECT_SOURCE_DIR}/src/app/main.cpp
  ${PROJECT_SOURCE_DIR}/src/app/cli.cpp
)
target_link_libraries(lwm_observation_probe PRIVATE liblwm ${CMAKE_DL_LIBS})
target_link_options(lwm_observation_probe PRIVATE
  "LINKER:--export-dynamic-symbol=xcb_get_property"
  "LINKER:--export-dynamic-symbol=xcb_get_property_reply")

# A real exec replacement that stops before starting the successor, allowing
# tests to change the display during the handoff without timing races.
add_executable(lwm_restart_gate restart_gate.cpp)
target_link_libraries(lwm_restart_gate PRIVATE lwm_build_options)
target_compile_definitions(lwm_restart_gate PRIVATE LWM_BINARY_PATH="$<TARGET_FILE:lwm>")
add_dependencies(lwm_restart_gate lwm)

add_executable(lwm_tests
  test_config_parser.cpp
  test_events_policy.cpp
  test_invariants.cpp
  test_state.cpp
  test_ewmh_classification.cpp
  test_ewmh_policy.cpp
  test_integration_config_reload.cpp
  test_focus_policy.cpp
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
  test_integration_placement.cpp
  test_floating_policy.cpp
  test_lwmctl.cpp
  test_ipc_server.cpp
  test_ipc_command.cpp
  test_notify_bridge.cpp
  test_harness.cpp
  test_workarea.cpp
  test_signal_lifecycle.cpp
  test_layout.cpp
  test_restart.cpp
  test_stacking_policy.cpp
  test_window_rules.cpp
  test_monitor_hotplug.cpp
  test_integration_drag.cpp
  test_integration_actions.cpp
  test_integration_transitions.cpp
  test_integration_hotplug.cpp
  test_workspace.cpp
  test_integration_logging.cpp
)

target_compile_definitions(lwm_tests PRIVATE
    LWM_BINARY_PATH="$<TARGET_FILE:lwm>"
    LWM_RESTART_GATE_PATH="$<TARGET_FILE:lwm_restart_gate>"
    LWM_RUNTIME_FAILURE_PROBE_PATH="$<TARGET_FILE:lwm_runtime_failure_probe>"
    LWM_OBSERVATION_PROBE_PATH="$<TARGET_FILE:lwm_observation_probe>"
    LWM_XORG_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/xorg-dummy.conf"
    LWM_NOTIFY_BRIDGE_PATH="${PROJECT_SOURCE_DIR}/scripts/lwm-notify-bridge.sh"
    LWMCTL_BINARY_PATH="$<TARGET_FILE:lwmctl>"
)
add_dependencies(lwm_tests lwm lwmctl lwm_runtime_failure_probe lwm_restart_gate lwm_observation_probe)

include(Catch)
foreach(target IN ITEMS lwm_tests lwm_logging_tests)
    target_link_libraries(${target} PRIVATE liblwm Catch2::Catch2WithMain PkgConfig::XCB_XTEST nlohmann_json::nlohmann_json)
    # Catch2 returns the failed-assertion count, so no exit code can mean skipped.
    catch_discover_tests(
        ${target} DISCOVERY_MODE PRE_TEST PROPERTIES SKIP_REGULAR_EXPRESSION "test cases: +1 \\| +0 passed \\| +1 skipped" TIMEOUT 60
    )
endforeach()

add_test(NAME uninstall_manifest
    COMMAND "${CMAKE_COMMAND}" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DUNINSTALL_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/uninstall.cmake"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/test_uninstall.cmake")
