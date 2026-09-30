cmake_minimum_required(VERSION 3.20)
project(lwm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Optimize for release
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

option(LWM_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
if(LWM_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-keysyms xcb-randr xcb-ewmh xcb-icccm xcb-sync x11)

pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

# Dependencies via FetchContent
include(FetchContent)

# toml++ for configuration
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)

# Quill 13.0.0: pin the reviewed source, including its bundled formatter.
FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG eb802a37c7d585840324886a3d8648c9c2159952
)
set(QUILL_DISABLE_NON_PREFIXED_MACROS ON CACHE BOOL "Use only project-prefixed logging macros" FORCE)
FetchContent_MakeAvailable(tomlplusplus quill)

add_subdirectory(src)

# Install under the configured CMAKE_INSTALL_PREFIX
install(TARGETS lwm lwmctl DESTINATION bin)
install(PROGRAMS scripts/lwm-notify.sh DESTINATION bin RENAME lwm-notify)
install(PROGRAMS scripts/lwm-notify-bridge.sh DESTINATION bin RENAME lwm-notify-bridge)

add_custom_target(uninstall
    COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${CMAKE_BINARY_DIR}/install_manifest.txt"
            -P "${CMAKE_SOURCE_DIR}/cmake/uninstall.cmake"
    VERBATIM
)

# Optional tests
option(BUILD_TESTS "Build unit and integration tests" OFF)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
