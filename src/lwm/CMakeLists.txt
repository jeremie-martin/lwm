add_library(liblwm STATIC
    core/connection.cpp
    core/ewmh.cpp
    core/floating.cpp
    core/focus.cpp
    core/window_rules.cpp
    config/config.cpp
    layout/layout.cpp
    keybind/keybind.cpp
    wm_focus.cpp
    wm_floating.cpp
    wm_workspace.cpp
    wm_ewmh.cpp
    wm.cpp
    wm_events.cpp
    wm_drag.cpp
)

target_include_directories(liblwm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(liblwm PUBLIC
    PkgConfig::XCB
    tomlplusplus::tomlplusplus
    spdlog::spdlog_header_only
)

# Set spdlog active level based on build type (compile-time filtering for zero-cost in release)
# SPDLOG_LEVEL_TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4, CRITICAL=5, OFF=6
target_compile_definitions(liblwm PUBLIC
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
    $<$<CONFIG:RelWithDebInfo>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>
)

set_target_properties(liblwm PROPERTIES OUTPUT_NAME lwm)
