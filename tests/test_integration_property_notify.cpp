#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>
#include <xcb/xcb_icccm.h>

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

std::vector<xcb_atom_t> get_window_property_atoms(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_ATOM_ATOM, 0, 64);
    auto* reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply)
        return {};

    std::vector<xcb_atom_t> result;
    auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
    int len = xcb_get_property_value_length(reply) / 4;
    result.assign(atoms, atoms + len);
    free(reply);
    return result;
}

bool property_has_atom(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t property, xcb_atom_t atom)
{
    auto atoms = get_window_property_atoms(conn, window, property);
    return std::ranges::find(atoms, atom) != atoms.end();
}

struct WindowGeometry
{
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t border_width = 0;

    bool operator==(WindowGeometry const&) const = default;
};

std::optional<WindowGeometry> get_window_geometry(X11Connection& conn, xcb_window_t window)
{
    auto cookie = xcb_get_geometry(conn.get(), window);
    auto* reply = xcb_get_geometry_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return std::nullopt;

    WindowGeometry result{
        .x = reply->x,
        .y = reply->y,
        .width = reply->width,
        .height = reply->height,
        .border_width = reply->border_width,
    };
    free(reply);
    return result;
}

bool is_active_window(X11Connection& conn, xcb_window_t expected)
{
    xcb_atom_t active = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    if (active == XCB_NONE)
        return false;

    auto value = get_window_property_window(conn.get(), conn.root(), active);
    return value && *value == expected;
}

bool is_stacked_above(X11Connection& conn, xcb_window_t upper, xcb_window_t lower)
{
    auto cookie = xcb_query_tree(conn.get(), conn.root());
    auto* reply = xcb_query_tree_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return false;

    int len = xcb_query_tree_children_length(reply);
    auto* children = xcb_query_tree_children(reply);
    auto* upper_it = std::find(children, children + len, upper);
    auto* lower_it = std::find(children, children + len, lower);
    bool result = upper_it != children + len && lower_it != children + len && upper_it > lower_it;
    free(reply);
    return result;
}

void set_transient_for(X11Connection& conn, xcb_window_t window, xcb_window_t parent)
{
    xcb_atom_t wm_transient_for = intern_atom(conn.get(), "WM_TRANSIENT_FOR");
    if (wm_transient_for == XCB_NONE)
        return;

    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, wm_transient_for, XCB_ATOM_WINDOW, 32, 1, &parent);
    xcb_flush(conn.get());
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
    if (net_wm_allowed_actions == XCB_NONE || net_wm_state == XCB_NONE || action_move == XCB_NONE
        || action_resize == XCB_NONE || state_skip_taskbar == XCB_NONE || state_skip_pager == XCB_NONE
        || state_above == XCB_NONE || type_utility == XCB_NONE || type_normal == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

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
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), window, net_wm_allowed_actions, action_move); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), window, net_wm_allowed_actions, action_resize); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), window, net_wm_state, state_skip_taskbar); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), window, net_wm_state, state_skip_pager); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), window, net_wm_state, state_above); }, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto sibling_geometry = get_window_geometry(conn, sibling);
            return sibling_geometry.has_value() && *sibling_geometry != *initial_sibling_geometry;
        },
        kTimeout
    ));

    set_window_type(conn, window, type_normal);
    REQUIRE(wait_for_condition([&]() { return !property_has_atom(conn.get(), window, net_wm_allowed_actions, action_move); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !property_has_atom(conn.get(), window, net_wm_allowed_actions, action_resize); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !property_has_atom(conn.get(), window, net_wm_state, state_skip_taskbar); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !property_has_atom(conn.get(), window, net_wm_state, state_skip_pager); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !property_has_atom(conn.get(), window, net_wm_state, state_above); }, kTimeout));
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
    if (net_wm_allowed_actions == XCB_NONE || net_wm_state == XCB_NONE || action_move == XCB_NONE
        || action_resize == XCB_NONE || state_skip_taskbar == XCB_NONE || state_skip_pager == XCB_NONE
        || net_wm_desktop == XCB_NONE || net_current_desktop == XCB_NONE)
    {
        WARN("Failed to intern atoms.");
        return;
    }

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
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), child, net_wm_allowed_actions, action_move); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), child, net_wm_allowed_actions, action_resize); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), child, net_wm_state, state_skip_taskbar); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), child, net_wm_state, state_skip_pager); }, kTimeout));
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
    REQUIRE(wait_for_condition([&]() { return !property_has_atom(conn.get(), child, net_wm_allowed_actions, action_move); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !property_has_atom(conn.get(), child, net_wm_allowed_actions, action_resize); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !property_has_atom(conn.get(), child, net_wm_state, state_skip_taskbar); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !property_has_atom(conn.get(), child, net_wm_state, state_skip_pager); }, kTimeout));
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
    if (net_active_window == XCB_NONE || net_wm_user_time == XCB_NONE || net_wm_user_time_window == XCB_NONE
        || net_wm_state == XCB_NONE || net_wm_state_demands_attention == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

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
    REQUIRE(wait_for_condition(
        [&]()
        {
            send_client_message(conn, w2, net_active_window, 1, 2500, 0, 0, 0);
            send_client_message(conn, w1, net_active_window, 1, 1500, 0, 0, 0);
            return is_active_window(conn, w2)
                && property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention);
        },
        kTimeout
    ));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
    destroy_window(conn, helper2);
    destroy_window(conn, helper1);
}

TEST_CASE("Integration: WM_HINTS.input changes can revoke focus eligibility", "[integration][property][focus][wm_hints]")
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

TEST_CASE("Integration: _NET_SUPPORTED does not overclaim visible-name atoms", "[integration][ewmh][root]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_supported = intern_atom(conn.get(), "_NET_SUPPORTED");
    xcb_atom_t net_wm_visible_name = intern_atom(conn.get(), "_NET_WM_VISIBLE_NAME");
    xcb_atom_t net_wm_visible_icon_name = intern_atom(conn.get(), "_NET_WM_VISIBLE_ICON_NAME");
    if (net_supported == XCB_NONE || net_wm_visible_name == XCB_NONE || net_wm_visible_icon_name == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    REQUIRE_FALSE(property_has_atom(conn.get(), conn.root(), net_supported, net_wm_visible_name));
    REQUIRE_FALSE(property_has_atom(conn.get(), conn.root(), net_supported, net_wm_visible_icon_name));

    xcb_window_t window = create_window(conn, 20, 20, 300, 200);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    REQUIRE_FALSE(get_window_property_string(conn.get(), window, net_wm_visible_name).has_value());
    REQUIRE_FALSE(get_window_property_string(conn.get(), window, net_wm_visible_icon_name).has_value());

    destroy_window(conn, window);
}
