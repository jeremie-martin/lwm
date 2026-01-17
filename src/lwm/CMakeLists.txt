add_library(liblwm STATIC
    core/connection.cpp
    core/ewmh.cpp
    core/focus.cpp
    config/config.cpp
    layout/layout.cpp
    keybind/keybind.cpp
    bar/bar.cpp
    wm.cpp
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
