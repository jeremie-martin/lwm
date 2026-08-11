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

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-keysyms xcb-randr xcb-ewmh xcb-icccm xcb-sync x11)

# Dependencies via FetchContent
include(FetchContent)

# toml++ for configuration
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)

# spdlog for logging (header-only mode for simplicity)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.0
)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
#include <format>
#if !defined(__cpp_lib_format) || __cpp_lib_format < 202207L
#error "C++23 std::format is unavailable"
#endif

int main()
{
    auto formatted = std::format("{}", 1);
    return formatted.empty();
}
]=] LWM_HAS_STD_FORMAT)

# Configure spdlog's own targets with the same format backend before they are
# created.  This matters for the default all-target build, which also builds
# spdlog's non-header-only target.
if(LWM_HAS_STD_FORMAT)
    set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "Use std::format in spdlog" FORCE)
endif()

FetchContent_MakeAvailable(tomlplusplus spdlog)

add_subdirectory(src)

# Install binaries to /usr/local/bin
install(TARGETS lwm lwmctl DESTINATION bin)
install(PROGRAMS scripts/lwm-notify.sh DESTINATION bin RENAME lwm-notify)

# Optional tests
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
