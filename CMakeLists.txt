cmake_minimum_required(VERSION 3.23)
project(lwm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

option(BUILD_TESTING "Build LWM tests" OFF)
option(LWM_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
include(CTest)
include(GNUInstallDirs)

# Usage requirements for LWM targets, without changing third-party builds.
add_library(lwm_build_options INTERFACE)
target_compile_features(lwm_build_options INTERFACE cxx_std_23)
if(LWM_SANITIZERS)
    target_compile_options(lwm_build_options INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(lwm_build_options INTERFACE -fsanitize=address,undefined)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-keysyms xcb-randr xcb-ewmh xcb-icccm xcb-sync x11)

pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

include(FetchContent)

# toml++ 3.4.0
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG 30172438cee64926dc41fdd9c11fb3ba5b2ba9de
)

# Quill 13.0.0: pin the reviewed source, including its bundled formatter.
FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG eb802a37c7d585840324886a3d8648c9c2159952
)
set(QUILL_DISABLE_NON_PREFIXED_MACROS ON)

# reflect-cpp 0.25.0 decodes the configuration schema using the same TOML parser.
FetchContent_Declare(
    reflectcpp
    GIT_REPOSITORY https://github.com/getml/reflect-cpp.git
    GIT_TAG 1327ef1d0b403f1424776da4c47bf0c7a6b0feaa
)
set(REFLECTCPP_JSON OFF)
set(REFLECTCPP_TOML ON)
set(REFLECTCPP_USE_VCPKG OFF)
FetchContent_MakeAvailable(tomlplusplus quill reflectcpp)

add_subdirectory(src/lwm)
add_subdirectory(src/app)

install(TARGETS lwm lwmctl DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS scripts/lwm-notify.sh DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME lwm-notify)
install(PROGRAMS scripts/lwm-notify-bridge.sh DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME lwm-notify-bridge)

add_custom_target(uninstall
    COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${PROJECT_BINARY_DIR}/install_manifest.txt"
            -P "${PROJECT_SOURCE_DIR}/cmake/uninstall.cmake"
    VERBATIM
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
