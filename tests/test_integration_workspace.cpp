#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

struct TestEnvironment
{
    X11TestEnvironment& x11_env;
    X11Connection conn;
    LwmProcess wm;

    bool ok() const { return conn.ok() && wm.running(); }

    static std::optional<TestEnvironment> create()
    {
        auto& env = X11TestEnvironment::instance();
        if (!env.available())
        {
            WARN("Xvfb not available; set LWM_TEST_ALLOW_EXISTING_DISPLAY=1 to use an existing DISPLAY.");
            return std::nullopt;
        }

        X11Connection conn;
        if (!conn.ok())
        {
            WARN("Failed to connect to X server.");
            return std::nullopt;
        }

        LwmProcess wm(env.display());
        if (!wm.running())
        {
            WARN("Failed to start lwm.");
            return std::nullopt;
        }

        if (!wait_for_wm_ready(conn, kTimeout))
        {
            WARN("Window manager not ready.");
            return std::nullopt;
        }

        return TestEnvironment{ env, std::move(conn), std::move(wm) };
    }
};

} // namespace

TEST_CASE("Integration: workspace switch updates _NET_CURRENT_DESKTOP", "[integration][workspace]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

    auto& conn = test_env->conn;

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
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

    auto& conn = test_env->conn;

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
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

    auto& conn = test_env->conn;

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

TEST_CASE(
    "Integration: fullscreen window maintains state across workspace switch",
    "[integration][workspace][fullscreen]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    if (net_current_desktop == XCB_NONE || net_wm_desktop == XCB_NONE || net_wm_state == XCB_NONE
        || net_wm_state_fullscreen == XCB_NONE || net_number_of_desktops == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    REQUIRE(num_desktops == 2);

    // Create window and make it fullscreen
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // Send _NET_WM_STATE_FULLSCREEN message to toggle fullscreen
    uint32_t values[5] = { 1, net_wm_state_fullscreen, 0, 0, 0 };
    send_client_message(conn, w1, net_wm_state, values[0], values[1], values[2], values[3], values[4]);

    // Verify fullscreen state is set
    auto check_fullscreen = [&]()
    {
        auto cookie = xcb_get_property(conn.get(), 0, w1, net_wm_state, XCB_ATOM_ATOM, 0, 10);
        auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
        if (!reply)
            return false;
        bool has_fullscreen = false;
        xcb_atom_t* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
        int len = xcb_get_property_value_length(reply) / 4;
        for (int i = 0; i < len; i++)
        {
            if (atoms[i] == net_wm_state_fullscreen)
                has_fullscreen = true;
        }
        free(reply);
        return has_fullscreen;
    };

    REQUIRE(wait_for_condition(check_fullscreen, kTimeout));

    // Get initial desktop
    uint32_t initial_desktop = get_window_property_cardinal(conn.get(), conn.root(), net_current_desktop).value_or(0);

    // Switch to workspace 1
    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    // Switch back to workspace 0
    send_client_message(conn, conn.root(), net_current_desktop, initial_desktop);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, initial_desktop, kTimeout));

    // Window should still be active and fullscreen
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(check_fullscreen());

    // Exit fullscreen
    values[0] = 0;
    send_client_message(conn, w1, net_wm_state, values[0], values[1], values[2], values[3], values[4]);

    // Verify fullscreen state is cleared
    auto check_not_fullscreen = [&]()
    {
        auto cookie = xcb_get_property(conn.get(), 0, w1, net_wm_state, XCB_ATOM_ATOM, 0, 10);
        auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
        if (!reply)
            return false;
        bool has_fullscreen = false;
        xcb_atom_t* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
        int len = xcb_get_property_value_length(reply) / 4;
        for (int i = 0; i < len; i++)
        {
            if (atoms[i] == net_wm_state_fullscreen)
                has_fullscreen = true;
        }
        free(reply);
        return !has_fullscreen;
    };

    REQUIRE(wait_for_condition(check_not_fullscreen, kTimeout));

    destroy_window(conn, w1);
}
