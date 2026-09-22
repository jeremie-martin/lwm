add_library(liblwm STATIC
    core/connection.cpp
    core/log.cpp
    core/ewmh.cpp
    core/floating.cpp
    core/focus.cpp
    core/ipc.cpp
    core/window_rules.cpp
    config/config.cpp
    layout/layout.cpp
    layout/split_tree.cpp
    layout/strategy.cpp
    keybind/keybind.cpp
    wm_rules.cpp
    wm_focus.cpp
    wm_floating.cpp
    wm_workspace.cpp
    wm_ewmh.cpp
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
    PkgConfig::XCB
    tomlplusplus::tomlplusplus
    spdlog::spdlog_header_only
)

# Keep every logging call available in every build; the owned logger applies runtime gates.
target_compile_definitions(liblwm PUBLIC
    LWM_VERSION="${PROJECT_VERSION}"
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
    SPDLOG_PREVENT_CHILD_FD
)

if(LWM_HAS_STD_FORMAT)
    # Clang 22 rejects spdlog 1.15's bundled fmt consteval checks. Use the
    # C++23 backend when the standard library provides the required format API.
    target_compile_definitions(liblwm PUBLIC SPDLOG_USE_STD_FORMAT)
endif()

set_target_properties(liblwm PROPERTIES OUTPUT_NAME lwm)
