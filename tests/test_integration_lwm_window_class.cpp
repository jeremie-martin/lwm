#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

bool wait_for_window_class(X11Connection& conn, xcb_atom_t atom, xcb_window_t window, std::string expected)
{
    return wait_for_condition(
        [&conn, atom, window, &expected]()
        {
            auto value = get_window_property_string(conn.get(), window, atom);
            return value.has_value() && *value == expected;
        },
        kTimeout
    );
}

} // namespace

TEST_CASE("Integration: tiled window publishes _LWM_WINDOW_CLASS = tiled", "[integration][ewmh][lwm_window_class]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t window = create_window(conn, 10, 10, 200, 150);
    map_window(conn, window);

    REQUIRE(wait_for_window_class(conn, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"), window, "tiled"));

    destroy_window(conn, window);
}

TEST_CASE("Integration: dialog window publishes _LWM_WINDOW_CLASS = floating", "[integration][ewmh][lwm_window_class]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t dialog_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    REQUIRE(dialog_type != XCB_NONE);

    xcb_window_t window = create_window(conn, 60, 60, 180, 120);
    set_window_type(conn, window, dialog_type);
    map_window(conn, window);

    REQUIRE(wait_for_window_class(conn, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"), window, "floating"));

    destroy_window(conn, window);
}

TEST_CASE("Integration: dock window publishes _LWM_WINDOW_CLASS = dock", "[integration][ewmh][lwm_window_class]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t dock_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DOCK");
    REQUIRE(dock_type != XCB_NONE);

    xcb_window_t window = create_window(conn, 0, 0, 1280, 24);
    set_window_type(conn, window, dock_type);
    map_window(conn, window);

    REQUIRE(wait_for_window_class(conn, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"), window, "dock"));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: _LWM_WINDOW_CLASS updates when a managed window changes type",
    "[integration][ewmh][lwm_window_class]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t window = create_window(conn, 10, 10, 200, 150);
    map_window(conn, window);

    REQUIRE(wait_for_window_class(conn, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"), window, "tiled"));

    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    REQUIRE(wait_for_window_class(conn, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"), window, "floating"));

    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_NORMAL"));
    REQUIRE(wait_for_window_class(conn, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"), window, "tiled"));

    destroy_window(conn, window);
}

TEST_CASE("Integration: the first recognized window type wins", "[integration][ewmh][classification]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto property = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE");
    auto published = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    auto normal = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_NORMAL");
    auto dialog = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    auto unknown = intern_atom(conn.get(), "_LWM_TEST_UNKNOWN_TYPE");
    REQUIRE(property != XCB_NONE);
    REQUIRE(published != XCB_NONE);
    REQUIRE(normal != XCB_NONE);
    REQUIRE(dialog != XCB_NONE);
    REQUIRE(unknown != XCB_NONE);
    for (auto const& [types, expected] : std::vector<std::pair<std::vector<xcb_atom_t>, std::string>>{
             { { unknown, normal, dialog },    "tiled" },
             { { unknown, dialog, normal }, "floating" },
             {                 { unknown },    "tiled" }
    })
    {
        auto window = create_window(conn, 10, 10, 200, 150);
        xcb_change_property(
            conn.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            property,
            XCB_ATOM_ATOM,
            32,
            types.size(),
            types.data()
        );
        map_window(conn, window);
        REQUIRE(wait_for_window_class(conn, published, window, expected));
        destroy_window(conn, window);
    }
}
