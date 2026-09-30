add_library(liblwm STATIC
    core/connection.cpp
    core/log.cpp
    core/ewmh.cpp
    core/floating.cpp
    core/focus.cpp
    core/ipc_command.cpp
    core/ipc_server.cpp
    core/restart.cpp
    core/ipc.cpp
    core/window_rules.cpp
    config/config.cpp
    layout/layout.cpp
    keybind/keybind.cpp
    wm_rules.cpp
    wm_focus.cpp
    wm_floating.cpp
    wm_workspace.cpp
    wm_ewmh.cpp
    wm_ipc.cpp
    wm_transition.cpp
    wm.cpp
    wm_events.cpp
    wm_drag.cpp
    wm_restart.cpp
    wm_scratchpad.cpp
)

target_include_directories(liblwm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(liblwm PUBLIC
    PkgConfig::SYSTEMD
    PkgConfig::XCB
    tomlplusplus::tomlplusplus
    quill::quill
)

# Keep every logging call available in every build; the owned logger applies runtime gates.
target_compile_definitions(liblwm PUBLIC
    LWM_VERSION="${PROJECT_VERSION}"
    QUILL_COMPILE_ACTIVE_LOG_LEVEL=QUILL_COMPILE_ACTIVE_LOG_LEVEL_TRACE_L3
    QUILL_ENABLE_IMMEDIATE_FLUSH=0
)

set_target_properties(liblwm PROPERTIES OUTPUT_NAME lwm)
