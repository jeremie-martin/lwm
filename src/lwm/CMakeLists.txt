add_library(liblwm STATIC
    core/action.cpp
    core/connection.cpp
    core/events.cpp
    core/signals.cpp
    core/workarea.cpp
    core/log.cpp
    core/classification.cpp
    core/ewmh.cpp
    core/floating.cpp
    core/stacking.cpp
    core/state.cpp
    core/geometry.cpp
    core/focus.cpp
    core/command.cpp
    core/ipc_server.cpp
    core/restart.cpp
    core/ipc.cpp
    core/window_rules.cpp
    config/config.cpp
    layout/layout.cpp
    wm_actions.cpp
    wm_manage.cpp
    wm_ipc.cpp
    wm_transition.cpp
    wm.cpp
    wm_events.cpp
    wm_drag.cpp
    wm_restart.cpp
    wm_scratchpad.cpp
)

target_include_directories(liblwm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(liblwm PUBLIC
    lwm_build_options
    PkgConfig::XCB
    quill::quill
    reflectcpp::reflectcpp
    PRIVATE PkgConfig::SYSTEMD
)

# Keep every logging call available in every build; the owned logger applies runtime gates.
target_compile_definitions(liblwm PUBLIC
    LWM_VERSION="${PROJECT_VERSION}"
    QUILL_COMPILE_ACTIVE_LOG_LEVEL=QUILL_COMPILE_ACTIVE_LOG_LEVEL_TRACE_L3
    QUILL_ENABLE_IMMEDIATE_FLUSH=0
)

set_target_properties(liblwm PROPERTIES OUTPUT_NAME lwm)
