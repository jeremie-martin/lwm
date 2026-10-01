/**
 * @file test_integration_focus_input.cpp
 * @brief Integration tests for X11 input focus correctness.
 *
 * These tests verify that the WM's _NET_ACTIVE_WINDOW property and the actual
 * X input focus (what determines keyboard delivery) stay in sync.  A divergence
 * means the window *looks* focused (border color) but the user cannot type in it.
 */

#include "wm_observations.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <optional>
#include <vector>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

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
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, wm_hints_atom, wm_hints_atom, 32, 9, hints);
    xcb_flush(conn.get());
}

/// Set WM_PROTOCOLS on a window (e.g., WM_TAKE_FOCUS, WM_DELETE_WINDOW).
void set_wm_protocols(X11Connection& conn, xcb_window_t window, std::initializer_list<xcb_atom_t> protocols)
{
    xcb_atom_t wm_protocols = intern_atom(conn.get(), "WM_PROTOCOLS");
    std::vector<xcb_atom_t> atoms(protocols);
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        window,
        wm_protocols,
        XCB_ATOM_ATOM,
        32,
        static_cast<uint32_t>(atoms.size()),
        atoms.data()
    );
    xcb_flush(conn.get());
}

} // namespace

// =============================================================================
// Sanity: for standard (Passive) windows, X input focus matches _NET_ACTIVE_WINDOW
// =============================================================================
TEST_CASE("Integration: X input focus matches _NET_ACTIVE_WINDOW for passive windows", "[integration][focus][input]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
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

TEST_CASE("Integration: focus delivers WM_TAKE_FOCUS and reasserts keyboard focus", "[integration][focus][input]")
{
    bool accepts_input = GENERATE(false, true);
    CAPTURE(accepts_input);
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto take_focus = intern_atom(conn.get(), "WM_TAKE_FOCUS");
    auto protocols = intern_atom(conn.get(), "WM_PROTOCOLS");
    auto active = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    auto window = create_window(conn, 10, 10, 200, 150);
    set_wm_hints_input(conn, window, accepts_input);
    set_wm_protocols(conn, window, { take_focus });
    auto messages = [&]
    {
        std::vector<xcb_client_message_event_t> result;
        while (auto* event = xcb_poll_for_event(conn.get()))
        {
            if ((event->response_type & ~0x80) == XCB_CLIENT_MESSAGE)
                result.push_back(*reinterpret_cast<xcb_client_message_event_t*>(event));
            free(event);
        }
        return result;
    };
    auto require_delivery = [&](std::optional<uint32_t> timestamp = std::nullopt)
    {
        REQUIRE(wait_for_x_input_focus(conn, window, kTimeout));
        std::vector<xcb_client_message_event_t> received;
        REQUIRE(wait_for_condition(
            [&]
            {
                received = messages();
                return !received.empty();
            },
            kTimeout
        ));
        REQUIRE(received.size() == 1);
        CHECK(received[0].response_type == (XCB_CLIENT_MESSAGE | 0x80));
        CHECK(received[0].window == window);
        CHECK(received[0].type == protocols);
        CHECK(received[0].format == 32);
        CHECK(received[0].data.data32[0] == take_focus);
        if (timestamp)
            CHECK(received[0].data.data32[1] == *timestamp);
    };
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    require_delivery();

    // Use real server timestamps from an unmapped client-owned window. Invented
    // future/stale values can make X reject SetInputFocus for an unrelated reason.
    auto clock_window = create_window(conn, 0, 0, 1, 1);
    auto marker = intern_atom(conn.get(), "_LWM_TEST_FOCUS_TIME");
    for (uint32_t iteration : { 1, 2 })
    {
        xcb_set_input_focus(conn.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn.root(), XCB_CURRENT_TIME);
        REQUIRE(wait_for_x_input_focus(conn, conn.root(), kTimeout));
        xcb_change_property(
            conn.get(), XCB_PROP_MODE_REPLACE, clock_window, marker, XCB_ATOM_CARDINAL, 32, 1, &iteration
        );
        xcb_flush(conn.get());
        std::optional<uint32_t> timestamp;
        REQUIRE(wait_for_condition(
            [&]
            {
                while (auto* event = xcb_poll_for_event(conn.get()))
                {
                    if ((event->response_type & ~0x80) == XCB_PROPERTY_NOTIFY)
                    {
                        auto const& property = *reinterpret_cast<xcb_property_notify_event_t*>(event);
                        if (property.window == clock_window && property.atom == marker)
                            timestamp = property.time;
                    }
                    free(event);
                }
                return timestamp.has_value();
            },
            kTimeout
        ));
        REQUIRE(*timestamp != XCB_CURRENT_TIME);
        send_client_message(conn, window, active, 1, *timestamp);
        require_delivery(timestamp);
        CHECK(wait_for_active_window(conn, window, kTimeout));
    }

    // Withdrawing the protocol must stop delivery. Only clients accepting direct
    // input remain eligible; otherwise completion repairs focus to the root.
    set_wm_protocols(conn, window, { });
    observe_title_after_events(conn, window);
    REQUIRE(wait_for_x_input_focus(conn, accepts_input ? window : conn.root(), kTimeout));
    if (accepts_input)
    {
        xcb_set_input_focus(conn.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn.root(), XCB_CURRENT_TIME);
        REQUIRE(wait_for_x_input_focus(conn, conn.root(), kTimeout));
        send_client_message(conn, window, active, 2);
        REQUIRE(wait_for_x_input_focus(conn, window, kTimeout));
    }
    CHECK(messages().empty());
    destroy_window(conn, clock_window);
    destroy_window(conn, window);
}

// =============================================================================
// Verify that _NET_ACTIVE_WINDOW and X focus agree after request-based focus
// changes (sanity check that our basic focus path is sound).
// =============================================================================
TEST_CASE("Integration: _NET_ACTIVE_WINDOW request produces matching X input focus", "[integration][focus][input]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
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
