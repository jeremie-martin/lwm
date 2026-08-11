include(FetchContent)
pkg_check_modules(XCB_XTEST REQUIRED IMPORTED_TARGET xcb-xtest)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.0
)
FetchContent_MakeAvailable(Catch2)

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
    test_split_tree.cpp
    test_stacking_policy.cpp
    test_window_rules.cpp
    test_monitor_hotplug.cpp
    test_workspace.cpp
    test_workspace_policy.cpp
    test_integration_logging.cpp
  )

target_link_libraries(lwm_tests PRIVATE liblwm Catch2::Catch2WithMain PkgConfig::XCB_XTEST)
target_include_directories(lwm_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(Catch)
catch_discover_tests(lwm_logging_tests PROPERTIES SKIP_RETURN_CODE 4)
catch_discover_tests(lwm_tests PROPERTIES SKIP_RETURN_CODE 4)
