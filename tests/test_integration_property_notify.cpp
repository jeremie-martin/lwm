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

    static std::optional<TestEnvironment> create(std::string const& config = {})
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

        LwmProcess wm(env.display(), config);
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

std::vector<xcb_window_t> get_window_property_windows(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_ATOM_WINDOW, 0, 64);
    auto* reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply)
        return {};

    std::vector<xcb_window_t> result;
    auto* windows = static_cast<xcb_window_t*>(xcb_get_property_value(reply));
    int len = xcb_get_property_value_length(reply) / 4;
    result.assign(windows, windows + len);
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

std::optional<uint32_t> get_wm_state(X11Connection& conn, xcb_window_t window, xcb_atom_t wm_state)
{
    auto cookie = xcb_get_property(conn.get(), 0, window, wm_state, wm_state, 0, 2);
    auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
    if (!reply || reply->type != wm_state || reply->format != 32 || xcb_get_property_value_length(reply) < 8)
    {
        free(reply);
        return std::nullopt;
    }

    uint32_t result = static_cast<uint32_t*>(xcb_get_property_value(reply))[0];
    free(reply);
    return result;
}

bool is_hidden_offscreen(X11Connection& conn, xcb_window_t window)
{
    auto geometry = get_window_geometry(conn, window);
    return geometry.has_value() && geometry->x < 0;
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

void set_window_desktop(X11Connection& conn, xcb_window_t window, uint32_t desktop)
{
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    if (net_wm_desktop == XCB_NONE)
        return;

    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, net_wm_desktop, XCB_ATOM_CARDINAL, 32, 1, &desktop);
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

void set_window_title(X11Connection& conn, xcb_window_t window, std::string const& title)
{
    xcb_atom_t net_wm_name = intern_atom(conn.get(), "_NET_WM_NAME");
    xcb_atom_t utf8_string = intern_atom(conn.get(), "UTF8_STRING");

    if (net_wm_name != XCB_NONE && utf8_string != XCB_NONE)
    {
        xcb_change_property(
            conn.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            net_wm_name,
            utf8_string,
            8,
            static_cast<uint32_t>(title.size()),
            title.data()
        );
    }

    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        window,
        XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING,
        8,
        static_cast<uint32_t>(title.size()),
        title.data()
    );
    xcb_flush(conn.get());
}

bool wait_for_window_geometry(
    X11Connection& conn,
    xcb_window_t window,
    int16_t x,
    int16_t y,
    uint16_t width,
    uint16_t height
)
{
    return wait_for_condition(
        [&conn, window, x, y, width, height]()
        {
            auto cookie = xcb_get_geometry(conn.get(), window);
            auto* reply = xcb_get_geometry_reply(conn.get(), cookie, nullptr);
            if (!reply)
                return false;

            bool matches = reply->x == x && reply->y == y && reply->width == width && reply->height == height;
            free(reply);
            return matches;
        },
        kTimeout
    );
}

std::string title_rule_geometry_config()
{
    return R"(
[workspaces]
count = 1
names = ["1"]

[[rules]]
match = { title = "micro" }
apply = { floating = true, center = true, geometry = { width = 400, height = 240 } }
)";
}

std::string title_rule_workspace_config()
{
    return R"(
[workspaces]
count = 2
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
count = 2
names = ["one", "two"]

[[rules]]
match = { type = "utility" }
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
    if (net_wm_allowed_actions == XCB_NONE || net_wm_state == XCB_NONE || action_move == XCB_NONE
        || action_resize == XCB_NONE || state_above == XCB_NONE || state_fullscreen == XCB_NONE
        || type_dialog == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t window = create_window(conn, 20, 20, 320, 220);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    send_client_message(conn, window, net_wm_state, 1, state_above, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), window, net_wm_state, state_above); }, kTimeout));

    send_client_message(conn, window, net_wm_state, 1, state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return property_has_atom(conn.get(), window, net_wm_state, state_fullscreen); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !property_has_atom(conn.get(), window, net_wm_state, state_above); }, kTimeout));

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

TEST_CASE(
    "Integration: WM_HINTS rewrite does not restore a user-iconified window",
    "[integration][property][wm_hints][wm_state]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    xcb_atom_t wm_state = intern_atom(conn.get(), "WM_STATE");
    xcb_atom_t wm_change_state = intern_atom(conn.get(), "WM_CHANGE_STATE");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    REQUIRE(wm_state != XCB_NONE);
    REQUIRE(wm_change_state != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_hidden != XCB_NONE);

    xcb_window_t window = create_window(conn, 10, 10, 220, 160);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(get_wm_state(conn, window, wm_state) == XCB_ICCCM_WM_STATE_NORMAL);

    send_client_message(conn, window, wm_change_state, XCB_ICCCM_WM_STATE_ICONIC);
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
        static_cast<int16_t>((conn.screen()->width_in_pixels - 410) / 2),
        static_cast<int16_t>((conn.screen()->height_in_pixels - 260) / 2),
        410,
        260
    ));

    set_window_desktop(conn, window, 0);
    REQUIRE(wait_for_property_cardinal(conn.get(), window, intern_atom(conn.get(), "_NET_WM_DESKTOP"), 0, kTimeout));

    set_wm_normal_hints(conn, window, 2000, 100, 410, 260);
    REQUIRE(wait_for_window_geometry(
        conn,
        window,
        static_cast<int16_t>((conn.screen()->width_in_pixels - 410) / 2),
        static_cast<int16_t>((conn.screen()->height_in_pixels - 260) / 2),
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
    auto require_realized_state_geometry = [&]()
    {
        REQUIRE(wait_for_window_geometry(
            conn,
            window,
            0,
            0,
            conn.screen()->width_in_pixels,
            conn.screen()->height_in_pixels
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
    require_realized_state_geometry();

    set_wm_normal_hints(conn, window, 140, 120, 410, 260);
    require_realized_state_geometry();
    REQUIRE(property_has_atom(conn.get(), window, net_wm_state, maximized_horz));
    REQUIRE(property_has_atom(conn.get(), window, net_wm_state, maximized_vert));

    send_client_message(conn, window, net_wm_state, 0, maximized_horz, maximized_vert);
    REQUIRE(wait_for_window_geometry(conn, window, 140, 120, 410, 260));

    send_client_message(conn, window, net_wm_state, 1, fullscreen);
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), window, net_wm_state, fullscreen); },
        kTimeout
    ));
    require_realized_state_geometry();

    set_wm_normal_hints(conn, window, 180, 150, 430, 290);
    require_realized_state_geometry();
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

    REQUIRE(wait_for_window_geometry(conn, window, 440, 240, 400, 240));

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
    uint32_t values[] = { 60, 60, 220, 160 };
    xcb_configure_window(
        conn.get(),
        window,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        values
    );
    xcb_flush(conn.get());

    REQUIRE(wait_for_window_geometry(conn, window, 440, 240, 400, 240));

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

TEST_CASE("Integration: initial and runtime rules share state precedence", "[integration][rules][map]")
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
apply = { borderless = true, sticky = false, fullscreen = false, above = false, skip_taskbar = false, skip_pager = false, floating = )"
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
        auto geometry = get_window_geometry(conn, window);
        auto atoms = get_window_property_atoms(conn.get(), window, state);
        return geometry && geometry->border_width == 0
            && std::ranges::none_of(
                   requested,
                   [&](auto atom) { return std::ranges::find(atoms, atom) != atoms.end(); }
            );
    };
    REQUIRE(wait_for_condition(correct_state, kTimeout));
    set_window_title(conn, window, "unruled");
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, window);
            return geometry && geometry->border_width == 4;
        },
        kTimeout
    ));
    set_window_title(conn, window, "ruled");
    REQUIRE(wait_for_condition(correct_state, kTimeout));
    destroy_window(conn, window);
}

TEST_CASE("Integration: rule reload preserves unspecified effective state", "[integration][rules][reload]")
{
    auto test_env = TestEnvironment::create(R"(
[[rules]]
apply = { floating = true, borderless = true, below = true, sticky = true, skip_taskbar = true, skip_pager = true }
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
    auto geometry = get_window_geometry(conn, window);
    REQUIRE(geometry.has_value());
    REQUIRE(geometry->border_width == 0);
    destroy_window(conn, window);
}
