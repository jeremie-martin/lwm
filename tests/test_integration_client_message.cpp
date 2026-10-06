#include "wm_observations.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

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
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

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
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_number_of_desktops != XCB_NONE);

    // Verify initial state (workspace 0)
    uint32_t num_desktops = require_property_cardinal(conn.get(), conn.root(), net_number_of_desktops);
    REQUIRE(num_desktops == 2);

    uint32_t w1_desktop = require_property_cardinal(conn.get(), w1, net_wm_desktop);
    uint32_t w2_desktop = require_property_cardinal(conn.get(), w2, net_wm_desktop);
    REQUIRE(w1_desktop == 0);
    REQUIRE(w2_desktop == 0);

    // Move w1 to workspace 1 (desktop index 1)
    send_net_wm_desktop(conn, w1, 1);

    REQUIRE(wait_for_property_cardinal(conn.get(), w1, net_wm_desktop, 1, kTimeout));

    w2_desktop = require_property_cardinal(conn.get(), w2, net_wm_desktop);
    REQUIRE(w2_desktop == 0);

    // Switch to workspace 1 to verify w1 is still there
    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // Switch back to workspace 0 to verify w2 is there
    send_client_message(conn, conn.root(), net_current_desktop, 0);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));

    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: move tiled window to out-of-range workspace is rejected",
    "[integration][client_message][workspace][edge]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& env = test_env->x11_env;
    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_number_of_desktops != XCB_NONE);

    uint32_t num_desktops = require_property_cardinal(conn.get(), conn.root(), net_number_of_desktops);
    uint32_t initial_desktop = require_property_cardinal(conn.get(), w1, net_wm_desktop);

    REQUIRE(num_desktops == 2);
    REQUIRE(initial_desktop == 0);

    // Try to move to non-existent workspace (desktop 99)
    send_net_wm_desktop(conn, w1, 99);

    observe_title_after_events(conn, w1);

    uint32_t final_desktop = require_property_cardinal(conn.get(), w1, net_wm_desktop);
    REQUIRE(final_desktop == initial_desktop);

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    destroy_window(conn, w1);
}

TEST_CASE("Integration: client message to invalid window ID is ignored", "[integration][client_message][edge]")
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& env = test_env->x11_env;
    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    // Create a real window
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");

    send_net_wm_desktop(conn, 0xDEADBEEF, 1);
    observe_title_after_events(conn, w1);

    auto ping = run_lwmctl(wm, { "version" });
    REQUIRE(ping.has_value());
    REQUIRE(ping->exit_code == 0);

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    destroy_window(conn, w1);
}

TEST_CASE("Integration: a _NET_WM_STATE too large for one request is not rewritten", "[integration][client_message][ewmh]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_window_t window = create_window(conn, 10, 10, 200, 150);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    // Foreign atoms, appended in chunks until the property exceeds one request.
    std::vector<xcb_atom_t> foreign(1U << 20, intern_atom(conn.get(), "_TEST_FOREIGN_STATE"));
    size_t words = 0;
    while (words <= xcb_get_maximum_request_length(conn.get()))
    {
        xcb_change_property(conn.get(), XCB_PROP_MODE_APPEND, window, net_wm_state, XCB_ATOM_ATOM, 32, foreign.size(), foreign.data());
        words += foreign.size();
    }
    // Activation reasserts focus, which merges LWM's atoms with the foreign ones.
    xcb_window_t other = create_window(conn, 10, 10, 200, 150);
    map_window(conn, other);
    REQUIRE(wait_for_active_window(conn, other, kTimeout));
    send_client_message(conn, window, intern_atom(conn.get(), "_NET_ACTIVE_WINDOW"), 2, XCB_CURRENT_TIME);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    auto alive = run_lwmctl(test_env->wm, { "version" });
    REQUIRE(alive);
    CHECK(alive->exit_code == 0);
    destroy_window(conn, other);
    destroy_window(conn, window);
}

TEST_CASE("Integration: close requests for LWM's own windows are ignored", "[integration][client_message][edge]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // The supporting window lacks WM_DELETE_WINDOW, so a close would kill LWM's connection.
    auto supporting = supporting_wm_window(conn);
    REQUIRE(supporting.has_value());
    send_client_message(conn, *supporting, intern_atom(conn.get(), "_NET_CLOSE_WINDOW"), XCB_CURRENT_TIME, 2);
    observe_title_after_events(conn, w1);

    auto ping = run_lwmctl(wm, { "version" });
    REQUIRE(ping.has_value());
    REQUIRE(ping->exit_code == 0);
    REQUIRE(supporting_wm_window(conn) == supporting);

    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: move focused window updates source workspace focus",
    "[integration][client_message][workspace][focus]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

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

    REQUIRE(wait_for_property_window(conn.get(), conn.root(), net_active_window, w1, kTimeout));

    // Switch to workspace 1 to verify w2 is there
    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: move window with desktop=0xFFFFFFFF sets sticky",
    "[integration][client_message][workspace][sticky]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& env = test_env->x11_env;
    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_sticky = intern_atom(conn.get(), "_NET_WM_STATE_STICKY");

    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_sticky != XCB_NONE);

    uint32_t initial_desktop = require_property_cardinal(conn.get(), w1, net_wm_desktop);

    // Move window to 0xFFFFFFFF to set sticky
    send_net_wm_desktop(conn, w1, 0xFFFFFFFF);

    REQUIRE(wait_for_property_cardinal(conn.get(), w1, net_wm_desktop, 0xFFFFFFFF, kTimeout));

    // Per EWMH spec, sticky windows have _NET_WM_DESKTOP = 0xFFFFFFFF
    uint32_t final_desktop = require_property_cardinal(conn.get(), w1, net_wm_desktop);
    REQUIRE(final_desktop == 0xFFFFFFFF);

    // _NET_WM_STATE is written after _NET_WM_DESKTOP; wait for it rather than racing it.
    REQUIRE(wait_for_condition([&] { return has_state(conn, w1, net_wm_state_sticky); }, kTimeout));

    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: invalid desktop request preserves sticky state",
    "[integration][client_message][workspace][sticky][edge]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state_sticky = intern_atom(conn.get(), "_NET_WM_STATE_STICKY");
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_wm_state_sticky != XCB_NONE);

    xcb_window_t window = create_window(conn, 10, 10, 200, 150);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    send_net_wm_desktop(conn, window, 0xFFFFFFFF);
    REQUIRE(wait_for_property_cardinal(conn.get(), window, net_wm_desktop, 0xFFFFFFFF, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, window, net_wm_state_sticky); }, kTimeout));

    send_net_wm_desktop(conn, window, 99);
    observe_title_after_events(conn, window);
    REQUIRE(require_property_cardinal(conn.get(), window, net_wm_desktop) == 0xFFFFFFFF);
    REQUIRE(has_state(conn, window, net_wm_state_sticky));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: _NET_ACTIVE_WINDOW honors _NET_WM_USER_TIME_WINDOW updates",
    "[integration][client_message][focus][user_time]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    xcb_atom_t net_wm_user_time = intern_atom(conn.get(), "_NET_WM_USER_TIME");
    xcb_atom_t net_wm_user_time_window = intern_atom(conn.get(), "_NET_WM_USER_TIME_WINDOW");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_active_window != XCB_NONE);
    REQUIRE(net_wm_user_time != XCB_NONE);
    REQUIRE(net_wm_user_time_window != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);

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

    // Application request with stale timestamp should be denied once the WM has observed the helper update.
    observe_title_after_events(conn, w2);
    send_client_message(conn, w1, net_active_window, 1, 1500, 0, 0, 0);
    // Attention is an observable consequence of rejection, not a preexisting state.
    REQUIRE(wait_for_condition([&] { return has_state(conn, w1, net_wm_state_demands_attention); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    send_client_message(conn, w1, net_active_window, 1, 2500, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    // Active-window publication precedes the deferred _NET_WM_STATE write.
    REQUIRE(wait_for_condition([&] { return !has_state(conn, w1, net_wm_state_demands_attention); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
    destroy_window(conn, user_time_window);
}

TEST_CASE(
    "Integration: _NET_ACTIVE_WINDOW source-1 timestamp zero is denied with attention",
    "[integration][client_message][focus][user_time]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    xcb_atom_t demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_active_window != XCB_NONE);
    REQUIRE(demands_attention != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 220, 160);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 60, 60, 220, 160);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    send_client_message(conn, w1, net_active_window, 1, 0, 0, 0, 0);

    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, demands_attention); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: accepted _NET_ACTIVE_WINDOW timestamp advances focus stealing user time",
    "[integration][client_message][focus][user_time]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    xcb_atom_t demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_active_window != XCB_NONE);
    REQUIRE(demands_attention != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 220, 160);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 60, 60, 220, 160);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    uint32_t accepted_time = 0xFFFFFF80u;
    uint32_t stale_time = 0xFFFFFF00u;
    send_client_message(conn, w1, net_active_window, 1, accepted_time, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    send_client_message(conn, w2, net_active_window, 1, stale_time, 0, 0, 0);

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w2, demands_attention); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: _NET_ACTIVE_WINDOW timestamps remain ordered across wraparound",
    "[integration][client_message][focus][user_time][wraparound]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    xcb_atom_t net_wm_user_time = intern_atom(conn.get(), "_NET_WM_USER_TIME");
    xcb_atom_t demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_active_window != XCB_NONE);
    REQUIRE(net_wm_user_time != XCB_NONE);
    REQUIRE(demands_attention != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 220, 160);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 60, 60, 220, 160);
    uint32_t initial_time = 0xFFFFFFD0U;
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        w2,
        net_wm_user_time,
        XCB_ATOM_CARDINAL,
        32,
        1,
        &initial_time
    );
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    send_client_message(conn, w1, net_active_window, 1, 0xFFFFFFF0U, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // 0x20 is later than 0xfffffff0 in X's signed-delta ordering.
    send_client_message(conn, w2, net_active_window, 1, 0x00000020U, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // The accepted post-wrap timestamp must also have advanced w2's fallback
    // user_time, making an older post-wrap request stale.
    send_client_message(conn, w1, net_active_window, 1, 0x00000010U, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, demands_attention); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: _NET_ACTIVE_WINDOW on iconic fullscreen-suppressed client sets attention",
    "[integration][client_message][focus][fullscreen][user_time]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    xcb_atom_t demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_active_window != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(fullscreen != XCB_NONE);
    REQUIRE(hidden != XCB_NONE);
    REQUIRE(demands_attention != XCB_NONE);

    xcb_window_t suppressed = create_window(conn, 80, 80, 320, 180);
    map_window(conn, suppressed);
    REQUIRE(wait_for_active_window(conn, suppressed, kTimeout));
    ipc_ok("scratchpad stash");
    REQUIRE(wait_for_condition([&]() { return has_state(conn, suppressed, hidden); }, kTimeout));

    xcb_window_t owner = create_window(conn, 10, 10, 640, 360);
    map_window(conn, owner);
    REQUIRE(wait_for_active_window(conn, owner, kTimeout));
    send_client_message(conn, owner, net_wm_state, 1, fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, owner, fullscreen); }, kTimeout));

    send_client_message(conn, suppressed, net_active_window, 1, 0xFFFFFF80u, 0, 0, 0);

    REQUIRE(wait_for_active_window(conn, owner, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, suppressed, demands_attention); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, suppressed, hidden); }, kTimeout));

    destroy_window(conn, suppressed);
    destroy_window(conn, owner);
}
