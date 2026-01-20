#include <catch2/catch_test_macros.hpp>
#include "x11_test_harness.hpp"
#include <chrono>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

bool ensure_environment()
{
    auto& env = X11TestEnvironment::instance();
    if (!env.available())
    {
        WARN("Xvfb not available; set LWM_TEST_ALLOW_EXISTING_DISPLAY=1 to use an existing DISPLAY.");
        return false;
    }
    return true;
}

} // namespace

TEST_CASE("Integration: tiled windows take focus in map order", "[integration][focus]")
{
    if (!ensure_environment())
        return;

    auto& env = X11TestEnvironment::instance();
    X11Connection conn;
    if (!conn.ok())
    {
        WARN("Failed to connect to X server.");
        return;
    }

    LwmProcess wm(env.display());
    if (!wm.running())
    {
        WARN("Failed to start lwm.");
        return;
    }

    REQUIRE(wait_for_wm_ready(conn, kTimeout));

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: focus restores to previous tiled window after destroy", "[integration][focus]")
{
    if (!ensure_environment())
        return;

    auto& env = X11TestEnvironment::instance();
    X11Connection conn;
    if (!conn.ok())
    {
        WARN("Failed to connect to X server.");
        return;
    }

    LwmProcess wm(env.display());
    if (!wm.running())
    {
        WARN("Failed to start lwm.");
        return;
    }

    REQUIRE(wait_for_wm_ready(conn, kTimeout));

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    destroy_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    destroy_window(conn, w1);
}

TEST_CASE("Integration: floating window grabs focus and yields on destroy", "[integration][focus][floating]")
{
    if (!ensure_environment())
        return;

    auto& env = X11TestEnvironment::instance();
    X11Connection conn;
    if (!conn.ok())
    {
        WARN("Failed to connect to X server.");
        return;
    }

    LwmProcess wm(env.display());
    if (!wm.running())
    {
        WARN("Failed to start lwm.");
        return;
    }

    REQUIRE(wait_for_wm_ready(conn, kTimeout));

    xcb_window_t tiled = create_window(conn, 10, 10, 200, 150);
    map_window(conn, tiled);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    xcb_atom_t dialog_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    if (dialog_type == XCB_NONE)
    {
        WARN("Failed to intern _NET_WM_WINDOW_TYPE_DIALOG.");
        destroy_window(conn, tiled);
        return;
    }

    xcb_window_t floating = create_window(conn, 60, 60, 180, 120);
    set_window_type(conn, floating, dialog_type);
    map_window(conn, floating);
    REQUIRE(wait_for_active_window(conn, floating, kTimeout));

    destroy_window(conn, floating);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    destroy_window(conn, tiled);
}
