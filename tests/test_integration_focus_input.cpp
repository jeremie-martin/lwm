/**
 * @file test_integration_focus_input.cpp
 * @brief Integration tests for X11 input focus correctness.
 *
 * These tests verify that the WM's _NET_ACTIVE_WINDOW property and the actual
 * X input focus (what determines keyboard delivery) stay in sync.  A divergence
 * means the window *looks* focused (border color) but the user cannot type in it.
 */

#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <vector>

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

/// Query the actual X server input focus (not the WM's _NET_ACTIVE_WINDOW).
xcb_window_t get_x_input_focus(X11Connection& conn)
{
    auto cookie = xcb_get_input_focus(conn.get());
    auto* reply = xcb_get_input_focus_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return XCB_NONE;
    xcb_window_t focus = reply->focus;
    free(reply);
    return focus;
}

/// Wait for xcb_get_input_focus() to return the expected window.
bool wait_for_x_input_focus(X11Connection& conn, xcb_window_t expected, std::chrono::milliseconds timeout)
{
    return wait_for_condition(
        [&]() { return get_x_input_focus(conn) == expected; },
        timeout
    );
}

/// Set WM_HINTS with the input field (ICCCM).
/// @param accepts_input  false for "Globally Active" windows that handle WM_TAKE_FOCUS themselves.
void set_wm_hints_input(X11Connection& conn, xcb_window_t window, bool accepts_input)
{
    // WM_HINTS layout: flags(CARD32), input(BOOL), initial_state, icon_pixmap,
    //   icon_window, icon_x, icon_y, icon_mask, window_group — total 9 CARD32 values.
    xcb_atom_t wm_hints_atom = intern_atom(conn.get(), "WM_HINTS");
    uint32_t hints[9] = {};
    hints[0] = 1; // InputHint flag
    hints[1] = accepts_input ? 1 : 0;
    xcb_change_property(
        conn.get(), XCB_PROP_MODE_REPLACE, window, wm_hints_atom, wm_hints_atom, 32, 9, hints
    );
    xcb_flush(conn.get());
}

/// Set WM_PROTOCOLS on a window (e.g., WM_TAKE_FOCUS, WM_DELETE_WINDOW).
void set_wm_protocols(X11Connection& conn, xcb_window_t window, std::initializer_list<xcb_atom_t> protocols)
{
    xcb_atom_t wm_protocols = intern_atom(conn.get(), "WM_PROTOCOLS");
    std::vector<xcb_atom_t> atoms(protocols);
    xcb_change_property(
        conn.get(), XCB_PROP_MODE_REPLACE, window, wm_protocols, XCB_ATOM_ATOM, 32,
        static_cast<uint32_t>(atoms.size()), atoms.data()
    );
    xcb_flush(conn.get());
}

} // namespace

// =============================================================================
// Sanity: for standard (Passive) windows, X input focus matches _NET_ACTIVE_WINDOW
// =============================================================================
TEST_CASE(
    "Integration: X input focus matches _NET_ACTIVE_WINDOW for passive windows",
    "[integration][focus][input]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;
    auto& conn = test_env->conn;

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    CHECK(wait_for_x_input_focus(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    CHECK(wait_for_x_input_focus(conn, w2, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

// =============================================================================
// Bug 1: WM must detect external focus changes (e.g., a client calling
//        XSetInputFocus) and update _NET_ACTIVE_WINDOW to match.
//
// Without FocusIn event handling, the WM's internal state and the actual X
// input focus can silently diverge: the red border shows window B, but
// keyboard input goes to window A.
// =============================================================================
TEST_CASE(
    "Integration: WM detects external focus change and updates _NET_ACTIVE_WINDOW",
    "[integration][focus][input]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;
    auto& conn = test_env->conn;

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // Externally change X input focus from w2 back to w1.
    // This simulates what a Globally Active client does after WM_TAKE_FOCUS,
    // or any application that calls XSetInputFocus directly.
    xcb_set_input_focus(conn.get(), XCB_INPUT_FOCUS_POINTER_ROOT, w1, XCB_CURRENT_TIME);
    xcb_flush(conn.get());

    // The WM should detect the focus change via FocusIn and update its state.
    CHECK(wait_for_active_window(conn, w1, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: WM tracks repeated external focus changes",
    "[integration][focus][input]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;
    auto& conn = test_env->conn;

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    xcb_window_t w3 = create_window(conn, 70, 70, 200, 150);

    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    map_window(conn, w3);
    REQUIRE(wait_for_active_window(conn, w3, kTimeout));

    // External focus change: w3 → w1
    xcb_set_input_focus(conn.get(), XCB_INPUT_FOCUS_POINTER_ROOT, w1, XCB_CURRENT_TIME);
    xcb_flush(conn.get());
    CHECK(wait_for_active_window(conn, w1, kTimeout));

    // External focus change: w1 → w2
    xcb_set_input_focus(conn.get(), XCB_INPUT_FOCUS_POINTER_ROOT, w2, XCB_CURRENT_TIME);
    xcb_flush(conn.get());
    CHECK(wait_for_active_window(conn, w2, kTimeout));

    destroy_window(conn, w3);
    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

// =============================================================================
// Bug 2: A "Globally Active" window (WM_HINTS.input=false, WM_TAKE_FOCUS in
//        WM_PROTOCOLS) should receive X input focus, not leave it on root.
//
// Per ICCCM the WM should let the client call SetInputFocus via WM_TAKE_FOCUS,
// but following dwm/i3 convention the WM should always set focus directly so
// that keyboard input isn't lost if the client is slow or ignores the message.
// =============================================================================
TEST_CASE(
    "Integration: globally active window receives X input focus",
    "[integration][focus][input]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;
    auto& conn = test_env->conn;

    xcb_atom_t wm_take_focus = intern_atom(conn.get(), "WM_TAKE_FOCUS");
    REQUIRE(wm_take_focus != XCB_NONE);

    // Create a "Globally Active" window: input=false, supports WM_TAKE_FOCUS
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    set_wm_hints_input(conn, w1, false);
    set_wm_protocols(conn, w1, { wm_take_focus });
    map_window(conn, w1);

    // The WM should set _NET_ACTIVE_WINDOW to w1
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // The actual X input focus should also be on w1 (not on root).
    // Before the fix, the WM sets focus to root for globally active windows.
    CHECK(wait_for_x_input_focus(conn, w1, kTimeout));

    destroy_window(conn, w1);
}

// =============================================================================
// Verify that _NET_ACTIVE_WINDOW and X focus agree after request-based focus
// changes (sanity check that our basic focus path is sound).
// =============================================================================
TEST_CASE(
    "Integration: _NET_ACTIVE_WINDOW request produces matching X input focus",
    "[integration][focus][input]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;
    auto& conn = test_env->conn;

    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    REQUIRE(net_active_window != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // Request focus via _NET_ACTIVE_WINDOW
    send_client_message(conn, w1, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    CHECK(wait_for_x_input_focus(conn, w1, kTimeout));

    // Back to w2
    send_client_message(conn, w2, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    CHECK(wait_for_x_input_focus(conn, w2, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}
