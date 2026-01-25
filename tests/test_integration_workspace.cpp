#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
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

TEST_CASE("Integration: workspace switch updates _NET_CURRENT_DESKTOP", "[integration][workspace]")
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

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    if (net_current_desktop == XCB_NONE || net_number_of_desktops == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    uint32_t initial_desktop = get_window_property_cardinal(conn.get(), conn.root(), net_current_desktop).value_or(0);

    REQUIRE(num_desktops == 2);
    REQUIRE(initial_desktop == 0);

    send_client_message(conn, conn.root(), net_current_desktop, 1);

    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));
}

TEST_CASE("Integration: workspace switch back and forth", "[integration][workspace]")
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

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    if (net_current_desktop == XCB_NONE || net_number_of_desktops == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    REQUIRE(num_desktops == 2);

    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    send_client_message(conn, conn.root(), net_current_desktop, 0);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));
}

TEST_CASE("Integration: windows persist across workspace switches", "[integration][workspace]")
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

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    if (net_current_desktop == XCB_NONE || net_wm_desktop == XCB_NONE || net_number_of_desktops == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    REQUIRE(num_desktops == 2);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    uint32_t initial_desktop = get_window_property_cardinal(conn.get(), conn.root(), net_current_desktop).value_or(0);
    uint32_t w1_desktop = get_window_property_cardinal(conn.get(), w1, net_wm_desktop).value_or(0);
    REQUIRE(w1_desktop == initial_desktop);

    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    w1_desktop = get_window_property_cardinal(conn.get(), w1, net_wm_desktop).value_or(0);
    REQUIRE(w1_desktop == initial_desktop);

    send_client_message(conn, conn.root(), net_current_desktop, initial_desktop);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, initial_desktop, kTimeout));
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    destroy_window(conn, w1);
}
