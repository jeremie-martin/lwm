add_library(liblwm STATIC
    core/connection.cpp
    core/ewmh.cpp
    core/floating.cpp
    core/focus.cpp
    config/config.cpp
    layout/layout.cpp
    keybind/keybind.cpp
    bar/bar.cpp
    wm_focus.cpp
    wm_floating.cpp
    wm_workspace.cpp
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
)

set_target_properties(liblwm PROPERTIES OUTPUT_NAME lwm)
