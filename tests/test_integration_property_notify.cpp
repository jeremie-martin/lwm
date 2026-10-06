#include "wm_observations.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <vector>
#include <xcb/xcb_icccm.h>
#include <xcb/xtest.h>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

bool is_active_window(X11Connection& conn, xcb_window_t expected)
{
    xcb_atom_t active = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    if (active == XCB_NONE)
        return false;

    auto value = get_window_property_window(conn.get(), conn.root(), active);
    return value && *value == expected;
}

bool is_listed_above(X11Connection& conn, xcb_window_t upper, xcb_window_t lower)
{
    xcb_atom_t stacking = intern_atom(conn.get(), "_NET_CLIENT_LIST_STACKING");
    if (stacking == XCB_NONE)
        return false;

    auto windows = get_window_property_windows(conn.get(), conn.root(), stacking);
    auto upper_it = std::ranges::find(windows, upper);
    auto lower_it = std::ranges::find(windows, lower);
    return upper_it != windows.end() && lower_it != windows.end() && upper_it > lower_it;
}

void clear_transient_for(X11Connection& conn, xcb_window_t window)
{
    xcb_atom_t wm_transient_for = intern_atom(conn.get(), "WM_TRANSIENT_FOR");
    if (wm_transient_for == XCB_NONE)
        return;

    xcb_delete_property(conn.get(), window, wm_transient_for);
    xcb_flush(conn.get());
}

void set_wm_input_hint(X11Connection& conn, xcb_window_t window, bool input)
{
    xcb_icccm_wm_hints_t hints = {};
    hints.flags = XCB_ICCCM_WM_HINT_INPUT;
    hints.input = input ? 1 : 0;
    xcb_icccm_set_wm_hints(conn.get(), window, &hints);
    xcb_flush(conn.get());
}

void set_wm_initial_state_and_urgency(X11Connection& conn, xcb_window_t window, uint32_t initial_state)
{
    constexpr uint32_t urgency_hint = 256;
    xcb_icccm_wm_hints_t hints = {};
    hints.flags = XCB_ICCCM_WM_HINT_STATE | urgency_hint;
    hints.initial_state = initial_state;
    xcb_icccm_set_wm_hints(conn.get(), window, &hints);
    xcb_flush(conn.get());
}

void set_wm_normal_hints(
    X11Connection& conn,
    xcb_window_t window,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height
)
{
    xcb_size_hints_t hints = {};
    hints.flags = XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_US_SIZE;
    hints.x = x;
    hints.y = y;
    hints.width = width;
    hints.height = height;
    xcb_icccm_set_wm_normal_hints(conn.get(), window, &hints);
    xcb_flush(conn.get());
}

void set_wm_protocols_take_focus(X11Connection& conn, xcb_window_t window)
{
    xcb_atom_t wm_protocols = intern_atom(conn.get(), "WM_PROTOCOLS");
    xcb_atom_t wm_take_focus = intern_atom(conn.get(), "WM_TAKE_FOCUS");
    if (wm_protocols == XCB_NONE || wm_take_focus == XCB_NONE)
        return;

    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, wm_protocols, XCB_ATOM_ATOM, 32, 1, &wm_take_focus);
    xcb_flush(conn.get());
}

void clear_wm_protocols(X11Connection& conn, xcb_window_t window)
{
    xcb_atom_t wm_protocols = intern_atom(conn.get(), "WM_PROTOCOLS");
    if (wm_protocols == XCB_NONE)
        return;

    xcb_delete_property(conn.get(), window, wm_protocols);
    xcb_flush(conn.get());
}

std::string title_rule_geometry_config()
{
    return R"(
[workspaces]
names = ["1"]

[[rules]]
match = { title = "micro" }
apply = { floating = true, geometry = { width = 400, height = 240 } }
)";
}

std::string title_rule_workspace_config()
{
    return R"(
[workspaces]
names = ["one", "two"]

[[rules]]
match = { title = "move-me" }
apply = { workspace = 1 }
)";
}

std::string type_rule_workspace_config()
{
    return R"(
[workspaces]
names = ["one", "two"]

[[rules]]
match = { type = "Utility" }
apply = { workspace = 1 }
)";
}

} // namespace

TEST_CASE("Integration: _NET_WM_WINDOW_TYPE changes reclassify managed windows", "[integration][property][ewmh]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_allowed_actions = intern_atom(conn.get(), "_NET_WM_ALLOWED_ACTIONS");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t action_move = intern_atom(conn.get(), "_NET_WM_ACTION_MOVE");
    xcb_atom_t action_resize = intern_atom(conn.get(), "_NET_WM_ACTION_RESIZE");
    xcb_atom_t state_skip_taskbar = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_TASKBAR");
    xcb_atom_t state_skip_pager = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_PAGER");
    xcb_atom_t state_above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");
    xcb_atom_t type_utility = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_UTILITY");
    xcb_atom_t type_normal = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_NORMAL");
    REQUIRE(net_wm_allowed_actions != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(action_move != XCB_NONE);
    REQUIRE(action_resize != XCB_NONE);
    REQUIRE(state_skip_taskbar != XCB_NONE);
    REQUIRE(state_skip_pager != XCB_NONE);
    REQUIRE(state_above != XCB_NONE);
    REQUIRE(type_utility != XCB_NONE);
    REQUIRE(type_normal != XCB_NONE);

    xcb_window_t window = create_window(conn, 20, 20, 300, 200);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    xcb_window_t sibling = create_window(conn, 360, 20, 300, 200);
    map_window(conn, sibling);
    REQUIRE(wait_for_active_window(conn, sibling, kTimeout));

    auto initial_window_geometry = get_window_geometry(conn, window);
    auto initial_sibling_geometry = get_window_geometry(conn, sibling);
    REQUIRE(initial_window_geometry.has_value());
    REQUIRE(initial_sibling_geometry.has_value());

    REQUIRE_FALSE(property_has_atom(conn.get(), window, net_wm_allowed_actions, action_move));
    REQUIRE_FALSE(property_has_atom(conn.get(), window, net_wm_allowed_actions, action_resize));

    set_window_type(conn, window, type_utility);
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), window, net_wm_allowed_actions, action_move); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), window, net_wm_allowed_actions, action_resize); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), window, net_wm_state, state_skip_taskbar); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), window, net_wm_state, state_skip_pager); },
        kTimeout
    ));
    REQUIRE(
        wait_for_condition([&]() { return property_has_atom(conn.get(), window, net_wm_state, state_above); }, kTimeout)
    );
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto sibling_geometry = get_window_geometry(conn, sibling);
            return sibling_geometry.has_value() && *sibling_geometry != *initial_sibling_geometry;
        },
        kTimeout
    ));

    set_window_type(conn, window, type_normal);
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), window, net_wm_allowed_actions, action_move); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), window, net_wm_allowed_actions, action_resize); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), window, net_wm_state, state_skip_taskbar); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), window, net_wm_state, state_skip_pager); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), window, net_wm_state, state_above); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto sibling_geometry = get_window_geometry(conn, sibling);
            return sibling_geometry.has_value() && *sibling_geometry == *initial_sibling_geometry;
        },
        kTimeout
    ));

    destroy_window(conn, sibling);
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: _NET_WM_WINDOW_TYPE changes reapply type-based workspace rules",
    "[integration][property][ewmh][rules][workspace]"
)
{
    auto test_env = TestEnvironment::create(type_rule_workspace_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t type_utility = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_UTILITY");
    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    REQUIRE(type_utility != XCB_NONE);
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);

    xcb_window_t fallback = create_window(conn, 10, 10, 220, 160);
    map_window(conn, fallback);
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    xcb_window_t window = create_window(conn, 60, 60, 220, 160);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(wait_for_property_cardinal(conn.get(), window, net_wm_desktop, 0, kTimeout));

    set_window_type(conn, window, type_utility);

    REQUIRE(wait_for_property_cardinal(conn.get(), window, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, window); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    destroy_window(conn, window);
    destroy_window(conn, fallback);
}

TEST_CASE(
    "Integration: fullscreen reevaluation keeps ABOVE cleared on window-type changes",
    "[integration][property][ewmh][fullscreen]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_allowed_actions = intern_atom(conn.get(), "_NET_WM_ALLOWED_ACTIONS");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t action_move = intern_atom(conn.get(), "_NET_WM_ACTION_MOVE");
    xcb_atom_t action_resize = intern_atom(conn.get(), "_NET_WM_ACTION_RESIZE");
    xcb_atom_t state_above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");
    xcb_atom_t state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t type_dialog = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    REQUIRE(net_wm_allowed_actions != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(action_move != XCB_NONE);
    REQUIRE(action_resize != XCB_NONE);
    REQUIRE(state_above != XCB_NONE);
    REQUIRE(state_fullscreen != XCB_NONE);
    REQUIRE(type_dialog != XCB_NONE);

    xcb_window_t window = create_window(conn, 20, 20, 320, 220);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    send_client_message(conn, window, net_wm_state, 1, state_above, 0, 0, 0);
    REQUIRE(
        wait_for_condition([&]() { return property_has_atom(conn.get(), window, net_wm_state, state_above); }, kTimeout)
    );

    send_client_message(conn, window, net_wm_state, 1, state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), window, net_wm_state, state_fullscreen); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), window, net_wm_state, state_above); },
        kTimeout
    ));

    REQUIRE_FALSE(property_has_atom(conn.get(), window, net_wm_allowed_actions, action_move));
    REQUIRE_FALSE(property_has_atom(conn.get(), window, net_wm_allowed_actions, action_resize));

    set_window_type(conn, window, type_dialog);
    REQUIRE(wait_for_condition(
        [&]()
        {
            return property_has_atom(conn.get(), window, net_wm_allowed_actions, action_move)
                && property_has_atom(conn.get(), window, net_wm_allowed_actions, action_resize)
                && property_has_atom(conn.get(), window, net_wm_state, state_fullscreen)
                && !property_has_atom(conn.get(), window, net_wm_state, state_above);
        },
        kTimeout
    ));

    destroy_window(conn, window);
}

TEST_CASE("Integration: WM_TRANSIENT_FOR changes reclassify managed windows", "[integration][property][transient]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_allowed_actions = intern_atom(conn.get(), "_NET_WM_ALLOWED_ACTIONS");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t action_move = intern_atom(conn.get(), "_NET_WM_ACTION_MOVE");
    xcb_atom_t action_resize = intern_atom(conn.get(), "_NET_WM_ACTION_RESIZE");
    xcb_atom_t state_skip_taskbar = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_TASKBAR");
    xcb_atom_t state_skip_pager = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_PAGER");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    REQUIRE(net_wm_allowed_actions != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(action_move != XCB_NONE);
    REQUIRE(action_resize != XCB_NONE);
    REQUIRE(state_skip_taskbar != XCB_NONE);
    REQUIRE(state_skip_pager != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_current_desktop != XCB_NONE);

    xcb_window_t parent = create_window(conn, 10, 10, 220, 160);
    map_window(conn, parent);
    REQUIRE(wait_for_active_window(conn, parent, kTimeout));

    xcb_window_t child = create_window(conn, 60, 60, 220, 160);
    map_window(conn, child);
    REQUIRE(wait_for_active_window(conn, child, kTimeout));

    auto initial_parent_geometry = get_window_geometry(conn, parent);
    REQUIRE(initial_parent_geometry.has_value());

    send_client_message(conn, child, net_wm_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), child, net_wm_desktop, 1, kTimeout));
    send_client_message(conn, conn.root(), net_current_desktop, 0);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));

    REQUIRE_FALSE(property_has_atom(conn.get(), child, net_wm_allowed_actions, action_move));
    REQUIRE_FALSE(property_has_atom(conn.get(), child, net_wm_state, state_skip_taskbar));

    set_transient_for(conn, child, parent);
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), child, net_wm_allowed_actions, action_move); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), child, net_wm_allowed_actions, action_resize); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), child, net_wm_state, state_skip_taskbar); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), child, net_wm_state, state_skip_pager); },
        kTimeout
    ));
    REQUIRE(wait_for_property_cardinal(conn.get(), child, net_wm_desktop, 0, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto parent_geometry = get_window_geometry(conn, parent);
            return parent_geometry.has_value() && *parent_geometry != *initial_parent_geometry;
        },
        kTimeout
    ));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, child, parent); }, kTimeout));

    clear_transient_for(conn, child);
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), child, net_wm_allowed_actions, action_move); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), child, net_wm_allowed_actions, action_resize); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), child, net_wm_state, state_skip_taskbar); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), child, net_wm_state, state_skip_pager); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto parent_geometry = get_window_geometry(conn, parent);
            return parent_geometry.has_value() && *parent_geometry == *initial_parent_geometry;
        },
        kTimeout
    ));

    destroy_window(conn, child);
    destroy_window(conn, parent);
}

TEST_CASE(
    "Integration: runtime transient restacking updates X stack and client-list stacking",
    "[integration][property][transient][stacking]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t type_dialog = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    REQUIRE(type_dialog != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

    xcb_window_t parent = create_window(conn, 10, 10, 260, 180);
    set_window_type(conn, parent, type_dialog);
    map_window(conn, parent);
    REQUIRE(wait_for_active_window(conn, parent, kTimeout));

    xcb_window_t child = create_window(conn, 60, 60, 220, 160);
    set_window_type(conn, child, type_dialog);
    map_window(conn, child);
    REQUIRE(wait_for_active_window(conn, child, kTimeout));

    send_client_message(conn, parent, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, parent, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, parent, child); }, kTimeout));

    set_transient_for(conn, child, parent);

    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, child, parent); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_listed_above(conn, child, parent); }, kTimeout));

    destroy_window(conn, child);
    destroy_window(conn, parent);
}

TEST_CASE(
    "Integration: _NET_WM_USER_TIME_WINDOW changes after manage update focus-stealing checks",
    "[integration][property][focus][user_time]"
)
{
    auto test_env = TestEnvironment::create();
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

    xcb_window_t helper1 = create_window(conn, -1000, -1000, 1, 1);
    xcb_window_t helper2 = create_window(conn, -1001, -1001, 1, 1);

    uint32_t initial_user_time = 100;
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        helper1,
        net_wm_user_time,
        XCB_ATOM_CARDINAL,
        32,
        1,
        &initial_user_time
    );

    xcb_window_t w2 = create_window(conn, 60, 60, 220, 160);
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        w2,
        net_wm_user_time_window,
        XCB_ATOM_WINDOW,
        32,
        1,
        &helper1
    );
    xcb_flush(conn.get());

    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        w2,
        net_wm_user_time_window,
        XCB_ATOM_WINDOW,
        32,
        1,
        &helper2
    );
    xcb_flush(conn.get());

    uint32_t updated_user_time = 2000;
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        helper2,
        net_wm_user_time,
        XCB_ATOM_CARDINAL,
        32,
        1,
        &updated_user_time
    );
    xcb_flush(conn.get());
    observe_title_after_events(conn, w2);
    send_client_message(conn, w1, net_active_window, 1, 1500, 0, 0, 0);
    // Attention is an observable consequence of rejection, not a preexisting state.
    REQUIRE(wait_for_condition([&] { return has_state(conn, w1, net_wm_state_demands_attention); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    send_client_message(conn, w1, net_active_window, 1, 2500, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&] { return !has_state(conn, w1, net_wm_state_demands_attention); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
    destroy_window(conn, helper2);
    destroy_window(conn, helper1);
}

TEST_CASE(
    "Integration: WM_HINTS.input changes can revoke focus eligibility",
    "[integration][property][focus][wm_hints]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t w1 = create_window(conn, 10, 10, 220, 160);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 60, 60, 220, 160);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    set_wm_input_hint(conn, w2, false);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: WM_HINTS rewrite does not restore a stashed window",
    "[integration][property][wm_hints][wm_state]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    xcb_atom_t wm_state = intern_atom(conn.get(), "WM_STATE");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    REQUIRE(wm_state != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_hidden != XCB_NONE);

    xcb_window_t window = create_window(conn, 10, 10, 220, 160);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(get_wm_state(conn, window, wm_state) == XCB_ICCCM_WM_STATE_NORMAL);

    ipc_ok("scratchpad stash");
    REQUIRE(wait_for_condition(
        [&]()
        {
            return get_wm_state(conn, window, wm_state) == XCB_ICCCM_WM_STATE_ICONIC
                && property_has_atom(conn.get(), window, net_wm_state, net_wm_state_hidden)
                && is_hidden_offscreen(conn, window);
        },
        kTimeout
    ));

    set_wm_initial_state_and_urgency(conn, window, XCB_ICCCM_WM_STATE_NORMAL);
    observe_title_after_events(conn, window);
    REQUIRE(wait_for_condition(
        [&]()
        {
            return get_wm_state(conn, window, wm_state) == XCB_ICCCM_WM_STATE_ICONIC
                && property_has_atom(conn.get(), window, net_wm_state, net_wm_state_hidden)
                && is_hidden_offscreen(conn, window);
        },
        kTimeout
    ));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: runtime WM_NORMAL_HINTS changes floating geometry and rejects invalid positions",
    "[integration][property][wm_normal_hints]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    xcb_atom_t dialog = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    REQUIRE(dialog != XCB_NONE);

    xcb_window_t window = create_window(conn, 10, 10, 220, 160);
    set_window_type(conn, window, dialog);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    set_wm_normal_hints(conn, window, 140, 120, 410, 260);
    REQUIRE(wait_for_window_geometry(conn, window, 140, 120, 410, 260));

    set_wm_normal_hints(conn, window, 2000, 100, 410, 260);
    REQUIRE(wait_for_window_geometry(
        conn,
        window,
        static_cast<int16_t>((conn.screen()->width_in_pixels - 414) / 2), // Centered frame
        static_cast<int16_t>((conn.screen()->height_in_pixels - 264) / 2),
        410,
        260
    ));

    set_window_desktop(conn, window, 0);
    REQUIRE(wait_for_property_cardinal(conn.get(), window, intern_atom(conn.get(), "_NET_WM_DESKTOP"), 0, kTimeout));

    set_wm_normal_hints(conn, window, 2000, 100, 410, 260);
    REQUIRE(wait_for_window_geometry(
        conn,
        window,
        static_cast<int16_t>((conn.screen()->width_in_pixels - 414) / 2), // Centered frame
        static_cast<int16_t>((conn.screen()->height_in_pixels - 264) / 2),
        410,
        260
    ));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: runtime WM_NORMAL_HINTS preserves maximize and fullscreen realization",
    "[integration][property][wm_normal_hints][wm_state]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    xcb_atom_t dialog = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t maximized_horz = intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_HORZ");
    xcb_atom_t maximized_vert = intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_VERT");
    xcb_atom_t fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    REQUIRE(dialog != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(maximized_horz != XCB_NONE);
    REQUIRE(maximized_vert != XCB_NONE);
    REQUIRE(fullscreen != XCB_NONE);

    xcb_window_t window = create_window(conn, 100, 90, 320, 220);
    // Maximize fills the workarea with the frame; fullscreen has no border.
    auto require_realized_state_geometry = [&](uint16_t border)
    {
        REQUIRE(wait_for_window_geometry(
            conn,
            window,
            0,
            0,
            conn.screen()->width_in_pixels - 2 * border,
            conn.screen()->height_in_pixels - 2 * border
        ));
    };

    set_window_type(conn, window, dialog);
    set_wm_normal_hints(conn, window, 100, 90, 320, 220);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(wait_for_window_geometry(conn, window, 100, 90, 320, 220));

    send_client_message(conn, window, net_wm_state, 1, maximized_horz, maximized_vert);
    REQUIRE(wait_for_condition(
        [&]()
        {
            return property_has_atom(conn.get(), window, net_wm_state, maximized_horz)
                && property_has_atom(conn.get(), window, net_wm_state, maximized_vert);
        },
        kTimeout
    ));
    require_realized_state_geometry(2);

    set_wm_normal_hints(conn, window, 140, 120, 410, 260);
    require_realized_state_geometry(2);
    REQUIRE(property_has_atom(conn.get(), window, net_wm_state, maximized_horz));
    REQUIRE(property_has_atom(conn.get(), window, net_wm_state, maximized_vert));

    send_client_message(conn, window, net_wm_state, 0, maximized_horz, maximized_vert);
    REQUIRE(wait_for_window_geometry(conn, window, 140, 120, 410, 260));

    send_client_message(conn, window, net_wm_state, 1, fullscreen);
    REQUIRE(
        wait_for_condition([&]() { return property_has_atom(conn.get(), window, net_wm_state, fullscreen); }, kTimeout)
    );
    require_realized_state_geometry(0);

    set_wm_normal_hints(conn, window, 180, 150, 430, 290);
    require_realized_state_geometry(0);
    REQUIRE(property_has_atom(conn.get(), window, net_wm_state, fullscreen));

    send_client_message(conn, window, net_wm_state, 0, fullscreen);
    REQUIRE(wait_for_window_geometry(conn, window, 180, 150, 430, 290));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: WM_PROTOCOLS changes can revoke active focus eligibility",
    "[integration][property][focus][wm_protocols]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t fallback = create_window(conn, 10, 10, 220, 160);
    map_window(conn, fallback);
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    xcb_window_t window = create_window(conn, 60, 60, 220, 160);
    set_wm_input_hint(conn, window, false);
    set_wm_protocols_take_focus(conn, window);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    clear_wm_protocols(conn, window);

    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    destroy_window(conn, window);
    destroy_window(conn, fallback);
}

TEST_CASE(
    "Integration: title rule geometry applies when title matches before map",
    "[integration][property][title][rules][map]"
)
{
    auto test_env = TestEnvironment::create(title_rule_geometry_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t window = create_window(conn, 60, 60, 220, 160);
    set_window_title(conn, window, "micro");
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    REQUIRE(wait_for_window_geometry(conn, window, 438, 238, 400, 240));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: title rule workspace applies when title matches before map",
    "[integration][property][title][rules][workspace][map]"
)
{
    auto test_env = TestEnvironment::create(title_rule_workspace_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);

    xcb_window_t window = create_window(conn, 60, 60, 220, 160);
    set_window_title(conn, window, "move-me");
    map_window(conn, window);

    REQUIRE(wait_for_property_cardinal(conn.get(), window, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: WM_NAME changes reapply title rule geometry for managed windows",
    "[integration][property][title][rules]"
)
{
    auto test_env = TestEnvironment::create(title_rule_geometry_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t window = create_window(conn, 60, 60, 220, 160);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    set_window_title(conn, window, "micro");
    REQUIRE(wait_for_window_geometry(conn, window, 438, 238, 400, 240));

    // A later application request supersedes the rule, even on its first attempt.
    uint32_t values[] = { 60, 60, 220, 160 };
    xcb_configure_window(
        conn.get(),
        window,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        values
    );
    xcb_flush(conn.get());

    REQUIRE(wait_for_window_geometry(conn, window, 60, 60, 220, 160));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: WM_NAME changes reapply title rule workspace for managed windows",
    "[integration][property][title][rules][workspace]"
)
{
    auto test_env = TestEnvironment::create(title_rule_workspace_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);

    xcb_window_t fallback = create_window(conn, 10, 10, 220, 160);
    map_window(conn, fallback);
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    xcb_window_t window = create_window(conn, 60, 60, 220, 160);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(wait_for_property_cardinal(conn.get(), window, net_wm_desktop, 0, kTimeout));

    set_window_title(conn, window, "move-me");

    REQUIRE(wait_for_property_cardinal(conn.get(), window, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, window); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    destroy_window(conn, window);
    destroy_window(conn, fallback);
}

TEST_CASE("Integration: _NET_SUPPORTED does not overclaim visible-name atoms", "[integration][ewmh][root]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_supported = intern_atom(conn.get(), "_NET_SUPPORTED");
    xcb_atom_t net_wm_visible_name = intern_atom(conn.get(), "_NET_WM_VISIBLE_NAME");
    xcb_atom_t net_wm_visible_icon_name = intern_atom(conn.get(), "_NET_WM_VISIBLE_ICON_NAME");
    REQUIRE(net_supported != XCB_NONE);
    REQUIRE(net_wm_visible_name != XCB_NONE);
    REQUIRE(net_wm_visible_icon_name != XCB_NONE);

    REQUIRE_FALSE(property_has_atom(conn.get(), conn.root(), net_supported, net_wm_visible_name));
    REQUIRE_FALSE(property_has_atom(conn.get(), conn.root(), net_supported, net_wm_visible_icon_name));

    xcb_window_t window = create_window(conn, 20, 20, 300, 200);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    REQUIRE_FALSE(get_window_property_string(conn.get(), window, net_wm_visible_name).has_value());
    REQUIRE_FALSE(get_window_property_string(conn.get(), window, net_wm_visible_icon_name).has_value());

    destroy_window(conn, window);
}

TEST_CASE("Integration: rules apply explicit actions without undo on match removal", "[integration][rules][map]")
{
    bool floating = false;
    SECTION("tiled") { floating = false; }
    SECTION("floating") { floating = true; }
    auto config = std::string(
                      R"(
[appearance]
border_width = 4
[[rules]]
match = { title = "ruled" }
apply = { borderless = true, sticky = false, fullscreen = false, layer = "normal", skip_taskbar = false, skip_pager = false, floating = )"
                  )
        + (floating ? "true" : "false") + " }\n";
    auto test_env = TestEnvironment::create(config);
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    std::vector<xcb_atom_t> requested;
    for (auto name : { "_NET_WM_STATE_STICKY",
                       "_NET_WM_STATE_FULLSCREEN",
                       "_NET_WM_STATE_ABOVE",
                       "_NET_WM_STATE_SKIP_TASKBAR",
                       "_NET_WM_STATE_SKIP_PAGER" })
        requested.push_back(intern_atom(conn.get(), name));

    auto window = create_window(conn, 60, 60, 220, 160);
    set_window_title(conn, window, "ruled");
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        window,
        state,
        XCB_ATOM_ATOM,
        32,
        requested.size(),
        requested.data()
    );
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    auto correct_state = [&]()
    {
        auto atoms = get_window_property_atoms(conn.get(), window, state);
        return get_window_border_width(conn, window) == 0
            && std::ranges::none_of(
                   requested,
                   [&](auto atom) { return std::ranges::find(atoms, atom) != atoms.end(); }
            );
    };
    REQUIRE(wait_for_condition(correct_state, kTimeout));
    // Losing a match does not undo its previous actions.
    observe_title_after_events(conn, window);
    REQUIRE(correct_state());
    set_window_title(conn, window, "ruled");
    REQUIRE(wait_for_condition(correct_state, kTimeout));
    destroy_window(conn, window);
}

TEST_CASE("Integration: rule reload preserves unspecified effective state", "[integration][rules][reload]")
{
    auto test_env = TestEnvironment::create(R"(
[[rules]]
apply = { floating = true, borderless = true, layer = "below", sticky = true, skip_taskbar = true, skip_pager = true }
)");
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;
    auto window = create_window(conn, 60, 60, 220, 160);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto taskbar = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_TASKBAR");
    REQUIRE(property_has_atom(conn.get(), window, state, taskbar));
    REQUIRE(test_env->wm.write_config("[[rules]]\napply = { skip_taskbar = false }\n"));
    auto reload = run_lwmctl(test_env->wm, { "reload-config" });
    REQUIRE(reload.has_value());
    REQUIRE(reload->exit_code == 0);
    REQUIRE_FALSE(property_has_atom(conn.get(), window, state, taskbar));
    for (auto name : { "_NET_WM_STATE_STICKY", "_NET_WM_STATE_BELOW", "_NET_WM_STATE_SKIP_PAGER" })
        REQUIRE(property_has_atom(conn.get(), window, state, intern_atom(conn.get(), name)));
    REQUIRE(get_window_border_width(conn, window) == 0);
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: class changes refresh classification and rule matching together",
    "[integration][property][rules]"
)
{
    auto env = TestEnvironment::create(R"(
[[rules]]
match = { class = "FloatNow" }
apply = { floating = true, borderless = true }
)");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 10, 10, 300, 200);
    set_window_wm_class(conn, window, "test", "TileNow");
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    auto kind = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    set_window_wm_class(conn, window, "test", "FloatNow");
    REQUIRE(
        wait_for_condition([&] { return get_window_property_string(conn.get(), window, kind) == "floating"; }, kTimeout)
    );
    set_window_wm_class(conn, window, "test", "TileNow");
    observe_title_after_events(conn, window);
    CHECK(get_window_property_string(conn.get(), window, kind) == "floating");
    CHECK(get_window_border_width(conn, window) == 0);
    destroy_window(conn, window);
}

// Frames saturate at the X11 extent; the window inside keeps the default border.
constexpr uint16_t kWidestFramedWindow = 65535 - 2 * 2;

TEST_CASE("Integration: oversized normal hints do not wrap floating dimensions", "[integration][property][bounds]")
{
    auto env = TestEnvironment::create(R"(
[[rules]]
match = { class = "HugeHints" }
apply = { floating = true }
)");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 10, 10, 300, 200);
    set_window_wm_class(conn, window, "test", "HugeHints");
    set_wm_normal_hints(conn, window, 10, 10, 65536, 32);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    auto geometry = get_window_geometry(conn, window);
    REQUIRE(geometry);
    CHECK(geometry->width == kWidestFramedWindow);
    CHECK(geometry->height == 32);
    set_wm_normal_hints(conn, window, 20, 20, 65537, 33);
    REQUIRE(wait_for_condition(
        [&]
        {
            auto g = get_window_geometry(conn, window);
            return g && g->height == 33;
        },
        kTimeout
    ));
    geometry = get_window_geometry(conn, window);
    REQUIRE(geometry);
    CHECK(geometry->width == kWidestFramedWindow);
    CHECK(geometry->height == 33);
    destroy_window(conn, window);
}

TEST_CASE("Integration: moveresize messages saturate geometry instead of wrapping", "[integration][bounds]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 10, 10, 300, 40);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    auto atom = intern_atom(conn.get(), "_NET_MOVERESIZE_WINDOW");
    REQUIRE(atom != XCB_NONE);
    auto resize = [&](uint32_t width, uint32_t height, uint16_t expected_width)
    {
        send_client_message(conn, window, atom, (1u << 10) | (1u << 11), 0, 0, width, height);
        REQUIRE(wait_for_condition(
            [&]
            {
                auto geometry = get_window_geometry(conn, window);
                return geometry && geometry->width == expected_width && geometry->height == height;
            },
            kTimeout
        ));
    };
    resize(65536, 41, kWidestFramedWindow);
    resize(UINT32_MAX, 42, kWidestFramedWindow);
    resize(0, 43, 1);
    send_client_message(conn, window, atom, (1u << 8) | (1u << 9), static_cast<uint32_t>(-40000), 40000);
    REQUIRE(wait_for_condition(
        [&]
        {
            auto geometry = get_window_geometry(conn, window);
            return geometry && geometry->x == -32768 && geometry->y == 32767;
        },
        kTimeout
    ));
    destroy_window(conn, window);
}

TEST_CASE("Integration: pointer resize saturates an oversized floating extent", "[integration][bounds][drag]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    if (!extension_available(conn, &xcb_test_id))
        SKIP("XTEST extension not available");
    auto window = create_window(conn, 0, 0, 65530, 50);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    auto geometry = get_window_geometry(conn, window);
    REQUIRE(geometry);
    REQUIRE(geometry->width == 65530);
    auto atom = intern_atom(conn.get(), "_NET_WM_MOVERESIZE");
    send_client_message(conn, window, atom, 100, 100, 4);
    observe_title_after_events(conn, window);
    xcb_test_fake_input(conn.get(), XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, conn.root(), 120, 100, 0);
    observe_title_after_events(conn, window);
    auto resized = get_window_geometry(conn, window);
    REQUIRE(resized);
    CHECK(resized->width == kWidestFramedWindow);
    CHECK(resized->height == 50);
    send_client_message(conn, window, atom, 120, 100, 11);
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: unchanged metadata rule results preserve user placement",
    "[integration][property][rules][metadata]"
)
{
    std::string config;
    SECTION("no rules") { }
    SECTION("unrelated rule") { config = "[[rules]]\nmatch = { class = 'Other' }\napply = { floating = true }\n"; }
    SECTION("same matching title rule")
    {
        config = "[[rules]]\nmatch = { title = 'work-.*' }\napply = { floating = true, geometry = { } }\n";
    }
    SECTION("same matching class rule")
    {
        config = "[[rules]]\nmatch = { class = 'Stable.*' }\napply = { floating = true, geometry = { } }\n";
    }
    auto env = TestEnvironment::create(config);
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 10, 10, 300, 200);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    set_window_title(conn, window, "work-one");
    set_window_wm_class(conn, window, "instance", "StableOne");
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    send_client_message(
        conn,
        window,
        intern_atom(conn.get(), "_NET_MOVERESIZE_WINDOW"),
        (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11),
        123,
        87,
        345,
        234
    );
    REQUIRE(wait_for_condition(
        [&]
        {
            auto geometry = get_window_geometry(conn, window);
            return geometry && geometry->x == 123 && geometry->y == 87 && geometry->width == 345;
        },
        kTimeout
    ));
    auto previous = get_window_geometry(conn, window);
    REQUIRE(previous);
    set_window_title(conn, window, "work-two");
    set_window_wm_class(conn, window, "instance", "StableTwo");
    REQUIRE(wait_for_condition(
        [&]
        {
            auto reply = send_ipc_command("window list");
            if (!reply || !reply->starts_with("ok "))
                return false;
            auto value = nlohmann::json::parse(reply->substr(3), nullptr, false);
            if (value.is_discarded())
                return false;
            auto const& windows = value.at("windows");
            return windows.size() == 1 && windows.at(0).at("id") == window && windows.at(0).at("title") == "work-two"
                && windows.at(0).at("class") == "StableTwo";
        },
        kTimeout
    ));
    CHECK(get_window_geometry(conn, window) == previous);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: malformed transient properties do not make clients floating",
    "[integration][property][malformed]"
)
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    auto parent = create_window(conn, 10, 10, 200, 150);
    auto child = create_window(conn, 20, 20, 200, 150);
    map_window(conn, parent);
    REQUIRE(wait_for_active_window(conn, parent, kTimeout));
    map_window(conn, child);
    REQUIRE(wait_for_active_window(conn, child, kTimeout));
    auto property = intern_atom(conn.get(), "WM_TRANSIENT_FOR");
    auto actions = intern_atom(conn.get(), "_NET_WM_ALLOWED_ACTIONS");
    auto move = intern_atom(conn.get(), "_NET_WM_ACTION_MOVE");
    auto check = [&](xcb_atom_t type, uint8_t format, uint32_t count)
    {
        set_transient_for(conn, child, parent);
        REQUIRE(wait_for_condition([&] { return property_has_atom(conn.get(), child, actions, move); }, kTimeout));
        uint32_t values[] = { parent, parent };
        xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, child, property, type, format, count, values);
        observe_title_after_events(conn, child);
        CHECK_FALSE(property_has_atom(conn.get(), child, actions, move));
    };
    check(XCB_ATOM_WINDOW, 8, 4);
    check(XCB_ATOM_WINDOW, 16, 2);
    check(XCB_ATOM_CARDINAL, 32, 1);
    check(XCB_ATOM_WINDOW, 32, 2);
    check(XCB_ATOM_WINDOW, 32, 0);
}

TEST_CASE(
    "Integration: malformed user timestamps cannot reject legitimate activation",
    "[integration][property][malformed]"
)
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    auto target = create_window(conn, 10, 10, 200, 150);
    auto active = create_window(conn, 20, 20, 200, 150);
    map_window(conn, target);
    REQUIRE(wait_for_active_window(conn, target, kTimeout));
    map_window(conn, active);
    REQUIRE(wait_for_active_window(conn, active, kTimeout));
    auto time = intern_atom(conn.get(), "_NET_WM_USER_TIME");
    auto activate = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    auto attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    uint32_t values[] = { 2000, 2000 };
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, active, time, XCB_ATOM_CARDINAL, 32, 1, values);
    observe_title_after_events(conn, active);
    send_client_message(conn, target, activate, 1, 1500);
    REQUIRE(wait_for_condition([&] { return has_state(conn, target, attention); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, active, kTimeout));
    auto check = [&](xcb_atom_t type, uint8_t format, uint32_t count)
    {
        xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, active, time, type, format, count, values);
        observe_title_after_events(conn, active);
        send_client_message(conn, target, activate, 1, 1500);
        REQUIRE(wait_for_active_window(conn, target, kTimeout));
        send_client_message(conn, active, activate, 2, 0);
        REQUIRE(wait_for_active_window(conn, active, kTimeout));
    };
    check(XCB_ATOM_CARDINAL, 8, 4);
    check(XCB_ATOM_CARDINAL, 16, 2);
    check(XCB_ATOM_WINDOW, 32, 1);
    check(XCB_ATOM_CARDINAL, 32, 2);
}

TEST_CASE("Integration: titles validate format and retain bounded text fallback", "[integration][property][malformed]")
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    auto window = create_window(conn, 10, 10, 200, 150);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    auto name = intern_atom(conn.get(), "_NET_WM_NAME");
    auto utf8 = intern_atom(conn.get(), "UTF8_STRING");
    std::string fallback = "legacy title";
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        window,
        XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING,
        8,
        fallback.size(),
        fallback.data()
    );
    auto check = [&](std::string const& expected)
    {
        xcb_flush(conn.get());
        REQUIRE(wait_for_condition(
            [&]
            {
                auto reply = send_ipc_command("window list");
                REQUIRE(reply);
                REQUIRE(reply->starts_with("ok "));
                auto snapshot = nlohmann::json::parse(reply->substr(3));
                for (auto const& entry : snapshot.at("windows"))
                    if (entry.at("id") == window && entry.at("title") == expected)
                        return true;
                return false;
            },
            kTimeout
        ));
    };
    uint32_t invalid = 0x41414141;
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, name, utf8, 32, 1, &invalid);
    check(fallback);
    std::string long_title(5000, 'b');
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, name, utf8, 8, long_title.size(), long_title.data());
    check(long_title.substr(0, 4096));
    xcb_delete_property(conn.get(), window, name);
    check(fallback);
}

TEST_CASE("Integration: admission subscribes before identity and dependent property reads", "[integration][property][observation]")
{
    bool adopting = GENERATE(false, true);
    auto& server = X11TestEnvironment::instance();
    if (!server.available()) SKIP("Test environment not available");
    X11Connection conn;
    REQUIRE(conn.ok());
    auto parent = create_window(conn, 10, 10, 200, 150);
    auto child = create_window(conn, 20, 20, 200, 150);
    auto helper = create_window(conn, -1000, -1000, 1, 1);
    auto time = intern_atom(conn.get(), "_NET_WM_USER_TIME");
    auto time_window = intern_atom(conn.get(), "_NET_WM_USER_TIME_WINDOW");
    auto type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE");
    uint32_t initial_time = 100;
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, helper, time, XCB_ATOM_CARDINAL, 32, 1, &initial_time);
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, child, time_window, XCB_ATOM_WINDOW, 32, 1, &helper);
    xcb_atom_t property = XCB_ATOM_WM_TRANSIENT_FOR, property_type = XCB_ATOM_WINDOW;
    xcb_window_t target = child;
    uint32_t value = parent;
    bool identity = true;
    SECTION("Transient identity") { }
    SECTION("Type identity")
    {
        property = type;
        property_type = XCB_ATOM_ATOM;
        value = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    }
    SECTION("Separate user-time window")
    {
        property = time;
        property_type = XCB_ATOM_CARDINAL;
        target = helper;
        value = 2000;
        identity = false;
    }
    SECTION("Root user-time window preserves redirection")
    {
        property = time;
        property_type = XCB_ATOM_CARDINAL;
        target = conn.root();
        value = 2000;
        identity = false;
        xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, target, time, XCB_ATOM_CARDINAL, 32, 1, &initial_time);
        xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, child, time_window, XCB_ATOM_WINDOW, 32, 1, &target);
    }
    // Keep the pointer outside all tile frames so crossing focus cannot alter user time.
    xcb_warp_pointer(conn.get(), XCB_NONE, conn.root(), 0, 0, 0, 0,
                     conn.screen()->width_in_pixels - 1, conn.screen()->height_in_pixels - 1);
    if (adopting)
    {
        map_window(conn, parent);
        map_window(conn, child);
    }
    REQUIRE(get_window_geometry(conn, child));
    LwmProcess wm(server.display(), "[workspaces]\nnames = [\"1\", \"2\"]\n", {}, -1, LWM_OBSERVATION_PROBE_PATH,
                  { { "LWM_TEST_WINDOW", std::to_string(target) },
                    { "LWM_TEST_PROPERTY", std::to_string(property) },
                    { "LWM_TEST_TYPE", std::to_string(property_type) },
                    { "LWM_TEST_VALUE", std::to_string(value) } });
    REQUIRE(wait_for_wm_ready(conn, kTimeout));
    if (!adopting)
    {
        map_window(conn, parent);
        REQUIRE(wait_for_active_window(conn, parent, kTimeout));
        map_window(conn, child);
    }
    REQUIRE(wait_for_active_window(conn, child, kTimeout));
    observe_title_after_events(conn, child);
    REQUIRE(read_property32(conn.get(), target, property, property_type) == std::optional{ std::vector<uint32_t>{ value } });
    if (identity)
        CHECK(ipc_json("window list").at("windows").at(1).at("kind") == "floating");
    else
    {
        auto active = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
        auto attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
        send_client_message(conn, parent, active, 1, 1500, 0, 0, 0);
        REQUIRE(wait_for_condition([&] { return has_state(conn, parent, attention); }, kTimeout));
        CHECK(is_active_window(conn, child));
        send_client_message(conn, parent, active, 1, 2500, 0, 0, 0);
        REQUIRE(wait_for_active_window(conn, parent, kTimeout));
        // A helper subscription must preserve root redirect and managed-window interest.
        auto newcomer = create_window(conn, 30, 30, 100, 100);
        map_window(conn, newcomer);
        REQUIRE(wait_for_active_window(conn, newcomer, kTimeout));
        destroy_window(conn, newcomer);
    }
    destroy_window(conn, child);
    destroy_window(conn, parent);
    destroy_window(conn, helper);
}
