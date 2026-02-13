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

// ─────────────────────────────────────────────────────────────────────────────
// Helper functions for sending _NET_WM_DESKTOP messages
// ─────────────────────────────────────────────────────────────────────────────

inline void send_net_wm_desktop(X11Connection& conn, xcb_window_t window, uint32_t desktop)
{
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    if (net_wm_desktop == XCB_NONE)
        return;

    send_client_message(conn, window, net_wm_desktop, desktop);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests for _NET_WM_DESKTOP message handling
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Integration: move tiled window to different workspace on same monitor",
    "[integration][client_message][workspace]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

    auto& env = test_env->x11_env;
    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    // Create windows on workspace 0
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);

    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // Get EWMH atoms
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    if (net_wm_desktop == XCB_NONE || net_number_of_desktops == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        destroy_window(conn, w2);
        destroy_window(conn, w1);
        return;
    }

    // Verify initial state (workspace 0)
    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    REQUIRE(num_desktops == 2);

    uint32_t w1_desktop = get_window_property_cardinal(conn.get(), w1, net_wm_desktop).value_or(0);
    uint32_t w2_desktop = get_window_property_cardinal(conn.get(), w2, net_wm_desktop).value_or(0);
    REQUIRE(w1_desktop == 0);
    REQUIRE(w2_desktop == 0);

    // Move w1 to workspace 1 (desktop index 1)
    send_net_wm_desktop(conn, w1, 1);

    // Verify w1's desktop property is updated
    REQUIRE(wait_for_property_cardinal(conn.get(), w1, net_wm_desktop, 1, kTimeout));

    // Verify w2's desktop property is unchanged
    w2_desktop = get_window_property_cardinal(conn.get(), w2, net_wm_desktop).value_or(0);
    REQUIRE(w2_desktop == 0);

    // Switch to workspace 1 to verify w1 is still there
    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    // w1 should be active on workspace 1
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // Switch back to workspace 0 to verify w2 is there
    send_client_message(conn, conn.root(), net_current_desktop, 0);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));

    // w2 should be active on workspace 0
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: move tiled window to out-of-range workspace is rejected",
    "[integration][client_message][workspace][edge]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

    auto& env = test_env->x11_env;
    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    if (net_wm_desktop == XCB_NONE || net_number_of_desktops == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        destroy_window(conn, w1);
        return;
    }

    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    uint32_t initial_desktop = get_window_property_cardinal(conn.get(), w1, net_wm_desktop).value_or(0);

    REQUIRE(num_desktops == 2);
    REQUIRE(initial_desktop == 0);

    // Try to move to non-existent workspace (desktop 99)
    send_net_wm_desktop(conn, w1, 99);

    // Wait a bit to ensure the message is processed
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Desktop property should remain unchanged (move rejected)
    uint32_t final_desktop = get_window_property_cardinal(conn.get(), w1, net_wm_desktop).value_or(0);
    REQUIRE(final_desktop == initial_desktop);

    // Window should still be accessible/focusable
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    destroy_window(conn, w1);
}

TEST_CASE("Integration: client message to invalid window ID is ignored", "[integration][client_message][edge]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

    auto& env = test_env->x11_env;
    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    // Create a real window
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");

    // Send client message to non-existent window - should be ignored without crash
    send_net_wm_desktop(conn, 0xDEADBEEF, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Real window should still be active and unaffected
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: move focused window updates source workspace focus",
    "[integration][client_message][workspace][focus]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

    auto& env = test_env->x11_env;
    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    // Create two windows on workspace 0
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);

    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");

    // Move focused window (w2) to workspace 1
    send_net_wm_desktop(conn, w2, 1);

    // Wait for desktop property update
    REQUIRE(wait_for_property_cardinal(conn.get(), w2, net_wm_desktop, 1, kTimeout));

    // w1 should become active on workspace 0
    REQUIRE(wait_for_property_window(conn.get(), conn.root(), net_active_window, w1, kTimeout));

    // Switch to workspace 1 to verify w2 is there
    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    // w2 should be active on workspace 1
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: move window with desktop=0xFFFFFFFF sets sticky",
    "[integration][client_message][workspace][sticky]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

    auto& env = test_env->x11_env;
    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_sticky = intern_atom(conn.get(), "_NET_WM_STATE_STICKY");

    if (net_wm_desktop == XCB_NONE || net_wm_state == XCB_NONE || net_wm_state_sticky == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        destroy_window(conn, w1);
        return;
    }

    uint32_t initial_desktop = get_window_property_cardinal(conn.get(), w1, net_wm_desktop).value_or(0);

    // Move window to 0xFFFFFFFF to set sticky
    send_net_wm_desktop(conn, w1, 0xFFFFFFFF);

    // Wait a bit for the message to be processed
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Window should still have its original desktop property (sticky windows don't have a specific desktop)
    uint32_t final_desktop = get_window_property_cardinal(conn.get(), w1, net_wm_desktop).value_or(0);
    REQUIRE(final_desktop == initial_desktop);

    // Window should have sticky state set
    auto cookie = xcb_get_property(conn.get(), 0, w1, net_wm_state, XCB_ATOM_ATOM, 0, 10);
    auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
    if (reply)
    {
        bool has_sticky = false;
        xcb_atom_t* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
        int len = xcb_get_property_value_length(reply) / 4;
        for (int i = 0; i < len; i++)
        {
            if (atoms[i] == net_wm_state_sticky)
                has_sticky = true;
        }
        free(reply);
        REQUIRE(has_sticky);
    }

    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: _NET_ACTIVE_WINDOW honors _NET_WM_USER_TIME_WINDOW updates",
    "[integration][client_message][focus][user_time]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

    auto& conn = test_env->conn;

    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    xcb_atom_t net_wm_user_time = intern_atom(conn.get(), "_NET_WM_USER_TIME");
    xcb_atom_t net_wm_user_time_window = intern_atom(conn.get(), "_NET_WM_USER_TIME_WINDOW");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    if (net_active_window == XCB_NONE || net_wm_user_time == XCB_NONE || net_wm_user_time_window == XCB_NONE
        || net_wm_state == XCB_NONE || net_wm_state_demands_attention == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    auto has_state = [&](xcb_window_t window, xcb_atom_t state)
    {
        auto cookie = xcb_get_property(conn.get(), 0, window, net_wm_state, XCB_ATOM_ATOM, 0, 16);
        auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
        if (!reply)
            return false;
        bool present = false;
        auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
        int len = xcb_get_property_value_length(reply) / 4;
        for (int i = 0; i < len; ++i)
        {
            if (atoms[i] == state)
            {
                present = true;
                break;
            }
        }
        free(reply);
        return present;
    };

    xcb_window_t w1 = create_window(conn, 10, 10, 220, 160);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // Unmapped helper window for _NET_WM_USER_TIME updates.
    xcb_window_t user_time_window = create_window(conn, -1000, -1000, 1, 1);

    xcb_window_t w2 = create_window(conn, 60, 60, 220, 160);
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        w2,
        net_wm_user_time_window,
        XCB_ATOM_WINDOW,
        32,
        1,
        &user_time_window
    );
    uint32_t initial_user_time = 100;
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        user_time_window,
        net_wm_user_time,
        XCB_ATOM_CARDINAL,
        32,
        1,
        &initial_user_time
    );
    xcb_flush(conn.get());

    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // Update user time after manage; WM should consume PropertyNotify from user_time_window.
    uint32_t updated_user_time = 2000;
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        user_time_window,
        net_wm_user_time,
        XCB_ATOM_CARDINAL,
        32,
        1,
        &updated_user_time
    );
    xcb_flush(conn.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Application request with stale timestamp should be denied.
    send_client_message(conn, w1, net_active_window, 1, 1500, 0, 0, 0);

    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(w1, net_wm_state_demands_attention); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
    destroy_window(conn, user_time_window);
}
