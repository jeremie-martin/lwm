#include "x11_test_harness.hpp"
#include <X11/Xlib.h>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <xcb/xcb_keysyms.h>
#include <xcb/xtest.h>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

struct WindowGeometry
{
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t border_width = 0;

    bool operator==(WindowGeometry const&) const = default;
};

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

bool has_state(X11Connection& conn, xcb_window_t window, xcb_atom_t state)
{
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    if (net_wm_state == XCB_NONE)
        return false;

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
}

void set_initial_window_state(X11Connection& conn, xcb_window_t window, std::initializer_list<xcb_atom_t> states)
{
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    if (net_wm_state == XCB_NONE)
        return;

    std::vector<xcb_atom_t> atoms(states);
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        window,
        net_wm_state,
        XCB_ATOM_ATOM,
        32,
        static_cast<uint32_t>(atoms.size()),
        atoms.data()
    );
    xcb_flush(conn.get());
}

void set_initial_desktop(X11Connection& conn, xcb_window_t window, uint32_t desktop)
{
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    if (net_wm_desktop == XCB_NONE)
        return;

    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, net_wm_desktop, XCB_ATOM_CARDINAL, 32, 1, &desktop);
    xcb_flush(conn.get());
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

bool is_hidden_offscreen(X11Connection& conn, xcb_window_t window)
{
    auto geometry = get_window_geometry(conn, window);
    return geometry.has_value() && geometry->x < 0;
}

bool property_has_atom(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t property, xcb_atom_t atom)
{
    auto cookie = xcb_get_property(conn, 0, window, property, XCB_ATOM_ATOM, 0, 64);
    auto* reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply)
        return false;

    bool present = false;
    auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
    int len = xcb_get_property_value_length(reply) / 4;
    for (int i = 0; i < len; ++i)
    {
        if (atoms[i] == atom)
        {
            present = true;
            break;
        }
    }
    free(reply);
    return present;
}

bool xtest_available(X11Connection& conn)
{
    auto* extension = xcb_get_extension_data(conn.get(), &xcb_test_id);
    return extension && extension->present;
}

std::optional<xcb_keycode_t> first_keycode_for_keysym(X11Connection& conn, xcb_keysym_t keysym)
{
    xcb_key_symbols_t* key_symbols = xcb_key_symbols_alloc(conn.get());
    if (!key_symbols)
        return std::nullopt;

    xcb_keycode_t* keycodes = xcb_key_symbols_get_keycode(key_symbols, keysym);
    std::optional<xcb_keycode_t> result;
    if (keycodes && keycodes[0] != XCB_NO_SYMBOL)
        result = keycodes[0];

    free(keycodes);
    xcb_key_symbols_free(key_symbols);
    return result;
}

bool send_mouse_chord(
    X11Connection& conn,
    xcb_keysym_t modifier,
    uint8_t button,
    int16_t root_x,
    int16_t root_y
)
{
    auto modifier_code = first_keycode_for_keysym(conn, modifier);
    if (!modifier_code)
        return false;

    xcb_test_fake_input(conn.get(), XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, conn.root(), root_x, root_y, 0);
    xcb_test_fake_input(conn.get(), XCB_KEY_PRESS, *modifier_code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_test_fake_input(conn.get(), XCB_BUTTON_PRESS, button, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_test_fake_input(conn.get(), XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_test_fake_input(conn.get(), XCB_KEY_RELEASE, *modifier_code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_flush(conn.get());
    return true;
}

} // namespace

TEST_CASE("Integration: tiled windows take focus in map order", "[integration][focus]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

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
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

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

TEST_CASE("Integration: tiled focus change restacks active window above sibling", "[integration][focus][stacking]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    if (net_active_window == XCB_NONE)
    {
        WARN("Failed to intern _NET_ACTIVE_WINDOW.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w2, w1); }, kTimeout));

    send_client_message(conn, w1, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w1, w2); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: _NET_RESTACK_WINDOW does not override managed stack policy", "[integration][focus][stacking]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_restack_window = intern_atom(conn.get(), "_NET_RESTACK_WINDOW");
    if (net_restack_window == XCB_NONE)
    {
        WARN("Failed to intern _NET_RESTACK_WINDOW.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w2, w1); }, kTimeout));

    send_client_message(conn, w1, net_restack_window, 2, w2, XCB_STACK_MODE_ABOVE, 0, 0);

    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w2, w1); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: focused tiled window does not restack above floating dialog", "[integration][focus][stacking]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t dialog_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    if (dialog_type == XCB_NONE || net_active_window == XCB_NONE)
    {
        WARN("Failed to intern required atoms.");
        return;
    }

    xcb_window_t tiled = create_window(conn, 10, 10, 200, 150);
    map_window(conn, tiled);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    xcb_window_t floating = create_window(conn, 60, 60, 180, 120);
    set_window_type(conn, floating, dialog_type);
    map_window(conn, floating);
    REQUIRE(wait_for_active_window(conn, floating, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, floating, tiled); }, kTimeout));

    send_client_message(conn, tiled, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);

    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, floating, tiled); }, kTimeout));

    destroy_window(conn, floating);
    destroy_window(conn, tiled);
}

TEST_CASE(
    "Integration: _NET_RESTACK_WINDOW on tiled window does not rise above floating dialog",
    "[integration][focus][stacking]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t dialog_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    xcb_atom_t net_restack_window = intern_atom(conn.get(), "_NET_RESTACK_WINDOW");
    if (dialog_type == XCB_NONE || net_restack_window == XCB_NONE)
    {
        WARN("Failed to intern required atoms.");
        return;
    }

    xcb_window_t tiled = create_window(conn, 10, 10, 200, 150);
    map_window(conn, tiled);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    xcb_window_t floating = create_window(conn, 60, 60, 180, 120);
    set_window_type(conn, floating, dialog_type);
    map_window(conn, floating);
    REQUIRE(wait_for_active_window(conn, floating, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, floating, tiled); }, kTimeout));

    send_client_message(conn, tiled, net_restack_window, 2, floating, XCB_STACK_MODE_ABOVE, 0, 0);

    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, floating, tiled); }, kTimeout));

    destroy_window(conn, floating);
    destroy_window(conn, tiled);
}

TEST_CASE("Integration: floating window grabs focus and yields on destroy", "[integration][focus][floating]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

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

TEST_CASE(
    "Integration: Super+Button2 on a managed window still reaches toggle_float",
    "[integration][focus][mouse]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    if (!xtest_available(conn))
        SKIP("XTEST extension not available");

    xcb_atom_t net_wm_allowed_actions = intern_atom(conn.get(), "_NET_WM_ALLOWED_ACTIONS");
    xcb_atom_t net_wm_action_move = intern_atom(conn.get(), "_NET_WM_ACTION_MOVE");
    REQUIRE(net_wm_allowed_actions != XCB_NONE);
    REQUIRE(net_wm_action_move != XCB_NONE);

    xcb_window_t window = create_window(conn, 10, 10, 320, 220);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE_FALSE(property_has_atom(conn.get(), window, net_wm_allowed_actions, net_wm_action_move));

    auto geometry = get_window_geometry(conn, window);
    REQUIRE(geometry.has_value());

    int16_t center_x = static_cast<int16_t>(geometry->x + geometry->width / 2);
    int16_t center_y = static_cast<int16_t>(geometry->y + geometry->height / 2);
    REQUIRE(send_mouse_chord(conn, XStringToKeysym("Super_L"), XCB_BUTTON_INDEX_2, center_x, center_y));

    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), window, net_wm_allowed_actions, net_wm_action_move); },
        kTimeout
    ));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: fullscreen window keeps zero border width when focus leaves and returns",
    "[integration][focus][fullscreen]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE || net_active_window == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // Add fullscreen state.
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);

    auto has_fullscreen_state = [&]()
    {
        auto cookie = xcb_get_property(conn.get(), 0, w1, net_wm_state, XCB_ATOM_ATOM, 0, 10);
        auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
        if (!reply)
            return false;
        bool has_fullscreen = false;
        auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
        int len = xcb_get_property_value_length(reply) / 4;
        for (int i = 0; i < len; ++i)
        {
            if (atoms[i] == net_wm_state_fullscreen)
            {
                has_fullscreen = true;
                break;
            }
        }
        free(reply);
        return has_fullscreen;
    };

    auto border_width_is_zero = [&]()
    {
        auto cookie = xcb_get_geometry(conn.get(), w1);
        auto* reply = xcb_get_geometry_reply(conn.get(), cookie, nullptr);
        if (!reply)
            return false;
        bool zero = (reply->border_width == 0);
        free(reply);
        return zero;
    };

    REQUIRE(wait_for_condition(has_fullscreen_state, kTimeout));
    REQUIRE(wait_for_condition(border_width_is_zero, kTimeout));

    // Map a sibling window; suppression model keeps the fullscreen owner active.
    xcb_window_t w2 = create_window(conn, 60, 60, 320, 180);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // Explicit focus request for fullscreen window (already active).
    send_client_message(conn, w1, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // Fullscreen and zero-border constraints should still hold after focus return.
    REQUIRE(wait_for_condition(has_fullscreen_state, kTimeout));
    REQUIRE(wait_for_condition(border_width_is_zero, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: floating fullscreen window keeps fullscreen geometry across restart",
    "[integration][focus][fullscreen][restart]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    xcb_atom_t dialog_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE
        || net_current_desktop == XCB_NONE || net_number_of_desktops == XCB_NONE || dialog_type == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    if (num_desktops < 2)
    {
        WARN("Need at least 2 workspaces for this test.");
        return;
    }

    xcb_window_t window = create_window(conn, 10, 10, 320, 220);
    set_window_type(conn, window, dialog_type);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    auto floating_geometry = get_window_geometry(conn, window);
    REQUIRE(floating_geometry.has_value());
    REQUIRE(floating_geometry->width < conn.screen()->width_in_pixels);
    REQUIRE(floating_geometry->height < conn.screen()->height_in_pixels);

    send_client_message(conn, window, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, window, net_wm_state_fullscreen); }, kTimeout));

    // Floating fullscreen geometry is applied when the window is shown again.
    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));
    send_client_message(conn, conn.root(), net_current_desktop, 0);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));

    REQUIRE(wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, window);
            return geometry.has_value() && geometry->x == 0 && geometry->y == 0
                && geometry->width == conn.screen()->width_in_pixels
                && geometry->height == conn.screen()->height_in_pixels;
        },
        kTimeout
    ));

    auto restart_result = run_lwmctl(wm, {"restart"});
    (void)restart_result;
    REQUIRE(wait_for_wm_ready(conn, std::chrono::seconds(5)));

    REQUIRE(wait_for_condition([&]() { return has_state(conn, window, net_wm_state_fullscreen); }, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, window);
            return geometry.has_value() && geometry->x == 0 && geometry->y == 0
                && geometry->width == conn.screen()->width_in_pixels
                && geometry->height == conn.screen()->height_in_pixels;
        },
        kTimeout
    ));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: second fullscreen window suppresses previous but preserves state",
    "[integration][focus][fullscreen][exclusive]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    auto has_fullscreen_state = [&](xcb_window_t window)
    {
        auto cookie = xcb_get_property(conn.get(), 0, window, net_wm_state, XCB_ATOM_ATOM, 0, 10);
        auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
        if (!reply)
            return false;
        bool has_fullscreen = false;
        auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
        int len = xcb_get_property_value_length(reply) / 4;
        for (int i = 0; i < len; ++i)
        {
            if (atoms[i] == net_wm_state_fullscreen)
            {
                has_fullscreen = true;
                break;
            }
        }
        free(reply);
        return has_fullscreen;
    };

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_fullscreen_state(w1); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 640, 360);
    map_window(conn, w2);
    // w2 is suppressed by w1's fullscreen; w1 stays active until w2 claims ownership

    send_client_message(conn, w2, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);

    REQUIRE(wait_for_condition([&]() { return has_fullscreen_state(w2); }, kTimeout));
    // Old fullscreen window keeps its state — it's suppressed, not stripped
    REQUIRE(wait_for_condition([&]() { return has_fullscreen_state(w1); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: map-time initial fullscreen request replaces current fullscreen owner",
    "[integration][focus][fullscreen][manage]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    if (net_wm_state_fullscreen == XCB_NONE)
    {
        WARN("Failed to intern _NET_WM_STATE_FULLSCREEN.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    send_client_message(conn, w1, intern_atom(conn.get(), "_NET_WM_STATE"), 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 640, 360);
    set_initial_window_state(conn, w2, { net_wm_state_fullscreen });
    map_window(conn, w2);

    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w2, net_wm_state_fullscreen); }, kTimeout));
    // Old fullscreen window keeps its state — suppressed, not stripped
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: _NET_ACTIVE_WINDOW ignores suppressed sibling and marks attention",
    "[integration][focus][fullscreen][activation]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE
        || net_wm_state_demands_attention == XCB_NONE || net_active_window == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 180);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    send_client_message(conn, w2, net_active_window, 1, XCB_CURRENT_TIME, 0, 0, 0);

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w2, net_wm_state_demands_attention); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: redirected focus keeps sticky fullscreen owner active after workspace switch",
    "[integration][focus][fullscreen][activation]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_wm_state_sticky = intern_atom(conn.get(), "_NET_WM_STATE_STICKY");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    if (net_current_desktop == XCB_NONE || net_wm_desktop == XCB_NONE || net_wm_state == XCB_NONE
        || net_wm_state_fullscreen == XCB_NONE || net_wm_state_sticky == XCB_NONE || net_active_window == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t owner = create_window(conn, 10, 10, 640, 360);
    map_window(conn, owner);
    REQUIRE(wait_for_active_window(conn, owner, kTimeout));

    send_client_message(conn, owner, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    send_client_message(conn, owner, net_wm_state, 1, net_wm_state_sticky, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, owner, net_wm_state_fullscreen); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, owner, net_wm_state_sticky); }, kTimeout));

    xcb_window_t target = create_window(conn, 80, 80, 320, 180);
    map_window(conn, target);
    REQUIRE(wait_for_active_window(conn, owner, kTimeout));

    send_client_message(conn, target, net_wm_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), target, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_active_window(conn, owner, kTimeout));

    send_client_message(conn, target, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);

    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));
    REQUIRE(wait_for_active_window(conn, owner, kTimeout));

    destroy_window(conn, target);
    destroy_window(conn, owner);
}

TEST_CASE(
    "Integration: fullscreen suppression stays scoped to the visible workspace",
    "[integration][focus][fullscreen][workspace]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    if (net_current_desktop == XCB_NONE || net_wm_desktop == XCB_NONE || net_wm_state == XCB_NONE
        || net_wm_state_fullscreen == XCB_NONE || net_active_window == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 180);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    send_client_message(conn, w2, net_wm_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), w2, net_wm_desktop, 1, kTimeout));

    send_client_message(conn, w2, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);

    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: suppressed floating sibling cannot restack above fullscreen owner",
    "[integration][focus][fullscreen][stacking]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 180);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w1, w2); }, kTimeout));

    uint32_t values[] = { w1, XCB_STACK_MODE_ABOVE };
    xcb_configure_window(
        conn.get(),
        w2,
        XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
        values
    );
    xcb_flush(conn.get());

    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w1, w2); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: sticky window on another workspace can take focus when mapped", "[integration][focus][sticky]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state_sticky = intern_atom(conn.get(), "_NET_WM_STATE_STICKY");
    if (net_active_window == XCB_NONE || net_current_desktop == XCB_NONE || net_wm_desktop == XCB_NONE
        || net_wm_state_sticky == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 220, 160);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t sticky = create_window(conn, 40, 40, 220, 160);
    map_window(conn, sticky);
    REQUIRE(wait_for_active_window(conn, sticky, kTimeout));

    send_client_message(conn, sticky, net_wm_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), sticky, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    send_client_message(conn, sticky, intern_atom(conn.get(), "_NET_WM_STATE"), 1, net_wm_state_sticky, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, sticky, net_wm_state_sticky); }, kTimeout));

    send_client_message(conn, sticky, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);

    REQUIRE(wait_for_active_window(conn, sticky, kTimeout));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));

    destroy_window(conn, sticky);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: iconifying a visible sticky window restores focus fallback", "[integration][focus][sticky]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state_hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    xcb_atom_t net_wm_state_sticky = intern_atom(conn.get(), "_NET_WM_STATE_STICKY");
    if (net_wm_state == XCB_NONE || net_wm_desktop == XCB_NONE || net_wm_state_hidden == XCB_NONE
        || net_wm_state_sticky == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t fallback = create_window(conn, 10, 10, 220, 160);
    map_window(conn, fallback);
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    xcb_window_t sticky = create_window(conn, 40, 40, 220, 160);
    map_window(conn, sticky);
    REQUIRE(wait_for_active_window(conn, sticky, kTimeout));

    send_client_message(conn, sticky, net_wm_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), sticky, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    send_client_message(conn, sticky, net_wm_state, 1, net_wm_state_sticky, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, sticky, net_wm_state_sticky); }, kTimeout));
    send_client_message(conn, sticky, intern_atom(conn.get(), "_NET_ACTIVE_WINDOW"), 2, XCB_CURRENT_TIME, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, sticky, kTimeout));

    send_client_message(conn, sticky, net_wm_state, 1, net_wm_state_hidden, 0, 0, 0);

    REQUIRE(wait_for_condition([&]() { return has_state(conn, sticky, net_wm_state_hidden); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    destroy_window(conn, sticky);
    destroy_window(conn, fallback);
}

TEST_CASE(
    "Integration: suppressed sibling is moved off-screen while fullscreen owner is active",
    "[integration][focus][fullscreen][visibility]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 180);
    map_window(conn, w2);

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, w2); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: _NET_RESTACK_WINDOW cannot raise suppressed sibling above fullscreen owner",
    "[integration][focus][fullscreen][stacking][diagnostic]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_restack_window = intern_atom(conn.get(), "_NET_RESTACK_WINDOW");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE || net_restack_window == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 180);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w1, w2); }, kTimeout));

    send_client_message(conn, w2, net_restack_window, 2, w1, XCB_STACK_MODE_ABOVE, 0, 0);

    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w1, w2); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: transient of suppressed sibling stays off-screen under fullscreen owner",
    "[integration][focus][fullscreen][transient]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t dialog_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    if (dialog_type == XCB_NONE || net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 180);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w3 = create_window(conn, 120, 120, 200, 120);
    set_window_type(conn, w3, dialog_type);
    set_transient_for(conn, w3, w2);
    map_window(conn, w3);

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, w3); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w1, w2); }, kTimeout));

    destroy_window(conn, w3);
    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: fullscreen loser is moved off-screen when a new owner takes over",
    "[integration][focus][fullscreen][handoff]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 640, 360);
    map_window(conn, w2);
    // w2 is suppressed by w1's fullscreen until w2 claims ownership via fullscreen request
    send_client_message(conn, w2, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);

    REQUIRE(wait_for_condition([&]() { return has_state(conn, w2, net_wm_state_fullscreen); }, kTimeout));
    // Old fullscreen window keeps state but is hidden off-screen (suppressed)
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, w1); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: suppressed floating fullscreen window regains ownership after owner exits",
    "[integration][focus][fullscreen][floating]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t dialog_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    if (dialog_type == XCB_NONE || net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 100, 120, 320, 180);
    set_window_type(conn, w1, dialog_type);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 20, 20, 640, 360);
    map_window(conn, w2);
    // w2 is suppressed by w1's fullscreen until w2 claims ownership via fullscreen request
    send_client_message(conn, w2, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);

    REQUIRE(wait_for_condition([&]() { return has_state(conn, w2, net_wm_state_fullscreen); }, kTimeout));
    // Old fullscreen window keeps state but is suppressed
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, w1); }, kTimeout));

    // After destroying the owner, the suppressed fullscreen window regains ownership
    destroy_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, w1); }, kTimeout));

    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: iconify and deiconify re-arbitrate fullscreen owner",
    "[integration][focus][fullscreen][iconify]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_wm_state_hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE || net_wm_state_hidden == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 180);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_hidden, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_hidden); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    send_client_message(conn, w1, net_wm_state, 0, net_wm_state_hidden, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w1, net_wm_state_hidden); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: exiting showing desktop restores fullscreen owner",
    "[integration][focus][fullscreen][showdesktop]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_showing_desktop = intern_atom(conn.get(), "_NET_SHOWING_DESKTOP");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE || net_showing_desktop == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 180);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    send_client_message(conn, conn.root(), net_showing_desktop, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send_client_message(conn, conn.root(), net_showing_desktop, 0);

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: clear_focus clears _NET_WM_STATE_FOCUSED from previous window", "[integration][focus][ewmh]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_focused = intern_atom(conn.get(), "_NET_WM_STATE_FOCUSED");
    xcb_atom_t net_showing_desktop = intern_atom(conn.get(), "_NET_SHOWING_DESKTOP");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    if (net_wm_state == XCB_NONE || net_wm_state_focused == XCB_NONE || net_showing_desktop == XCB_NONE
        || net_active_window == XCB_NONE)
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

    xcb_window_t w1 = create_window(conn, 20, 20, 320, 200);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(w1, net_wm_state_focused); }, kTimeout));

    // Trigger clear_focus() via showing desktop mode.
    send_client_message(conn, conn.root(), net_showing_desktop, 1);
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto value = get_window_property_window(conn.get(), conn.root(), net_active_window);
            return value && *value == XCB_NONE;
        },
        kTimeout
    ));

    // Exit showing desktop and focus another window.
    send_client_message(conn, conn.root(), net_showing_desktop, 0);

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 200);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    REQUIRE(wait_for_condition([&]() { return !has_state(w1, net_wm_state_focused); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(w2, net_wm_state_focused); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: transient dialog stacks above its parent", "[integration][transient][stacking]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    // Create a tiled parent and map it.
    xcb_window_t parent = create_window(conn, 10, 10, 300, 200);
    map_window(conn, parent);
    REQUIRE(wait_for_active_window(conn, parent, kTimeout));

    // Create a transient dialog: set WM_TRANSIENT_FOR before mapping so the WM
    // recognises it as floating from the start.
    xcb_window_t transient = create_window(conn, 50, 50, 200, 150);
    set_transient_for(conn, transient, parent);
    map_window(conn, transient);
    REQUIRE(wait_for_active_window(conn, transient, kTimeout));

    // The transient must be stacked above its parent.
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, transient, parent); }, kTimeout));

    destroy_window(conn, transient);
    destroy_window(conn, parent);
}

TEST_CASE(
    "Integration: transient follows parent visibility across workspace switch",
    "[integration][transient][workspace]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    if (net_current_desktop == XCB_NONE || net_number_of_desktops == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    if (num_desktops < 2)
    {
        WARN("Need at least 2 workspaces for this test.");
        return;
    }

    // Start on workspace 0.
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));

    // Create a tiled parent on ws0 and map it.
    xcb_window_t parent = create_window(conn, 10, 10, 300, 200);
    map_window(conn, parent);
    REQUIRE(wait_for_active_window(conn, parent, kTimeout));

    // Create a transient for the parent and map it.
    xcb_window_t transient = create_window(conn, 50, 50, 200, 150);
    set_transient_for(conn, transient, parent);
    map_window(conn, transient);
    REQUIRE(wait_for_active_window(conn, transient, kTimeout));

    // Switch to workspace 1.
    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    // Both parent and transient should be hidden off-screen.
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, parent); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, transient); }, kTimeout));

    // Switch back to workspace 0.
    send_client_message(conn, conn.root(), net_current_desktop, 0);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));

    // Both parent and transient should be visible (not off-screen).
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, parent); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, transient); }, kTimeout));

    destroy_window(conn, transient);
    destroy_window(conn, parent);
}

TEST_CASE(
    "Integration: multiple transients of same parent all stack above it",
    "[integration][transient][stacking]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    // Create a tiled parent and map it.
    xcb_window_t parent = create_window(conn, 10, 10, 300, 200);
    map_window(conn, parent);
    REQUIRE(wait_for_active_window(conn, parent, kTimeout));

    // Create first transient dialog, set WM_TRANSIENT_FOR before mapping.
    xcb_window_t transient1 = create_window(conn, 40, 40, 180, 120);
    set_transient_for(conn, transient1, parent);
    map_window(conn, transient1);
    REQUIRE(wait_for_active_window(conn, transient1, kTimeout));

    // Create second transient dialog, set WM_TRANSIENT_FOR before mapping.
    xcb_window_t transient2 = create_window(conn, 70, 70, 180, 120);
    set_transient_for(conn, transient2, parent);
    map_window(conn, transient2);
    REQUIRE(wait_for_active_window(conn, transient2, kTimeout));

    // Both transients must be stacked above the parent.
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, transient1, parent); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, transient2, parent); }, kTimeout));

    destroy_window(conn, transient2);
    destroy_window(conn, transient1);
    destroy_window(conn, parent);
}
