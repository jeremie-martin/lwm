#include "x11_test_harness.hpp"
#include <X11/Xlib.h>
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>
#include <xcb/xcb_keysyms.h>
#include <xcb/xtest.h>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

struct WindowGeometry
{
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    bool operator==(WindowGeometry const&) const = default;
};

std::optional<WindowGeometry> get_window_geometry(X11Connection& conn, xcb_window_t window)
{
    auto cookie = xcb_get_geometry(conn.get(), window);
    auto* reply = xcb_get_geometry_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return std::nullopt;
    WindowGeometry g { reply->x, reply->y, reply->width, reply->height };
    free(reply);
    return g;
}

bool intersects_root(X11Connection& conn, WindowGeometry const& geometry)
{
    int32_t const left = std::max<int32_t>(0, geometry.x);
    int32_t const top = std::max<int32_t>(0, geometry.y);
    int32_t const right = std::min<int32_t>(conn.screen()->width_in_pixels, geometry.x + geometry.width);
    int32_t const bottom = std::min<int32_t>(conn.screen()->height_in_pixels, geometry.y + geometry.height);
    return left < right && top < bottom;
}

bool window_is_viewable(X11Connection& conn, xcb_window_t window)
{
    auto cookie = xcb_get_window_attributes(conn.get(), window);
    auto* reply = xcb_get_window_attributes_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return false;
    bool const result = reply->map_state == XCB_MAP_STATE_VIEWABLE;
    free(reply);
    return result;
}

std::optional<xcb_window_t> get_input_focus(X11Connection& conn)
{
    auto cookie = xcb_get_input_focus(conn.get());
    auto* reply = xcb_get_input_focus_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return std::nullopt;
    xcb_window_t const result = reply->focus;
    free(reply);
    return result;
}

void synchronize_x_server(X11Connection& conn)
{
    auto cookie = xcb_get_input_focus(conn.get());
    auto* reply = xcb_get_input_focus_reply(conn.get(), cookie, nullptr);
    free(reply);
}

bool drain_window_visibility_events(X11Connection& conn, xcb_window_t window)
{
    bool observed = false;
    while (auto* event = xcb_poll_for_event(conn.get()))
    {
        uint8_t const response_type = event->response_type & 0x7f;
        if (response_type == XCB_MAP_NOTIFY)
        {
            auto* map = reinterpret_cast<xcb_map_notify_event_t*>(event);
            observed = observed || map->window == window;
        }
        else if (response_type == XCB_UNMAP_NOTIFY)
        {
            auto* unmap = reinterpret_cast<xcb_unmap_notify_event_t*>(event);
            observed = observed || unmap->window == window;
        }
        free(event);
    }
    return observed;
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

    uint32_t const result = static_cast<uint32_t*>(xcb_get_property_value(reply))[0];
    free(reply);
    return result;
}

std::optional<bool> property_contains_atom(
    X11Connection& conn,
    xcb_window_t window,
    xcb_atom_t property,
    xcb_atom_t expected
)
{
    auto cookie = xcb_get_property(conn.get(), 0, window, property, XCB_ATOM_ATOM, 0, 64);
    auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return std::nullopt;
    if (reply->type == XCB_ATOM_NONE && reply->format == 0 && xcb_get_property_value_length(reply) == 0)
    {
        free(reply);
        return false;
    }
    if (reply->type != XCB_ATOM_ATOM || reply->format != 32)
    {
        free(reply);
        return std::nullopt;
    }

    bool result = false;
    auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
    int const count = xcb_get_property_value_length(reply) / 4;
    for (int i = 0; i < count; ++i)
        result = result || atoms[i] == expected;
    free(reply);
    return result;
}

std::optional<std::vector<xcb_window_t>> get_client_list(X11Connection& conn, xcb_atom_t client_list)
{
    auto cookie = xcb_get_property(conn.get(), 0, conn.root(), client_list, XCB_ATOM_WINDOW, 0, 4096);
    auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
    if (!reply || reply->type != XCB_ATOM_WINDOW || reply->format != 32)
    {
        free(reply);
        return std::nullopt;
    }

    int const count = xcb_get_property_value_length(reply) / 4;
    auto* windows = static_cast<xcb_window_t*>(xcb_get_property_value(reply));
    std::vector<xcb_window_t> result(windows, windows + count);
    std::sort(result.begin(), result.end());
    free(reply);
    return result;
}

std::optional<std::string> wait_for_ipc_socket_path(X11Connection& conn)
{
    xcb_atom_t socket_atom = intern_atom(conn.get(), "_LWM_IPC_SOCKET");
    if (socket_atom == XCB_NONE)
        return std::nullopt;
    bool ready = wait_for_condition(
        [&conn, socket_atom]()
        {
            auto value = get_window_property_string(conn.get(), conn.root(), socket_atom);
            return value && !value->empty();
        },
        kTimeout);
    if (!ready)
        return std::nullopt;
    return get_window_property_string(conn.get(), conn.root(), socket_atom);
}

bool lwmctl_available()
{
    return std::filesystem::exists(lwmctl_executable_path());
}

struct TestEnvironment
{
    X11TestEnvironment& x11_env;
    X11Connection conn;
    LwmProcess wm;

    bool ok() const { return conn.ok() && wm.running(); }

    static std::optional<TestEnvironment> create(std::string config = "[workspaces]\ncount = 2\n")
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

        LwmProcess wm(env.display(), std::move(config));
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

bool send_key_chord(X11Connection& conn, xcb_keysym_t modifier, xcb_keysym_t key)
{
    auto modifier_code = first_keycode_for_keysym(conn, modifier);
    auto key_code = first_keycode_for_keysym(conn, key);
    if (!modifier_code || !key_code)
        return false;

    xcb_test_fake_input(conn.get(), XCB_KEY_PRESS, *modifier_code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_test_fake_input(conn.get(), XCB_KEY_PRESS, *key_code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_test_fake_input(conn.get(), XCB_KEY_RELEASE, *key_code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_test_fake_input(conn.get(), XCB_KEY_RELEASE, *modifier_code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_flush(conn.get());
    return true;
}

} // namespace

TEST_CASE("Integration: workspace switch updates _NET_CURRENT_DESKTOP", "[integration][workspace]")
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
        SKIP("Test environment not available");

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
        SKIP("Test environment not available");

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

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(check_fullscreen());

    // Exit fullscreen
    values[0] = 0;
    send_client_message(conn, w1, net_wm_state, values[0], values[1], values[2], values[3], values[4]);

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

// =============================================================================
// Desktop move: moving the focused window to a hidden workspace via
// _NET_WM_DESKTOP should transfer focus to a remaining window.
// =============================================================================
TEST_CASE(
    "Integration: moving focused window to hidden workspace preserves normal mapped state",
    "[integration][workspace][focus][visibility]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    xcb_atom_t net_client_list = intern_atom(conn.get(), "_NET_CLIENT_LIST");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    xcb_atom_t wm_state = intern_atom(conn.get(), "WM_STATE");
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_number_of_desktops != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);
    REQUIRE(net_client_list != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_hidden != XCB_NONE);
    REQUIRE(wm_state != XCB_NONE);

    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    if (num_desktops < 2)
        SKIP("Need at least 2 desktops");

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    uint32_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn.get(), w2, XCB_CW_EVENT_MASK, &event_mask);
    xcb_flush(conn.get());
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto focus = get_input_focus(conn);
            return focus && *focus == w2;
        },
        kTimeout
    ));
    synchronize_x_server(conn);
    drain_window_visibility_events(conn, w2);

    auto client_list_before = get_client_list(conn, net_client_list);
    REQUIRE(client_list_before.has_value());
    REQUIRE(std::binary_search(client_list_before->begin(), client_list_before->end(), w1));
    REQUIRE(std::binary_search(client_list_before->begin(), client_list_before->end(), w2));
    REQUIRE(get_wm_state(conn, w2, wm_state) == XCB_ICCCM_WM_STATE_NORMAL);

    // Move w2 to desktop 1 while desktop 0 remains current.
    send_client_message(conn, w2, net_wm_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), w2, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto current = get_window_property_cardinal(conn.get(), conn.root(), net_current_desktop);
            auto geometry = get_window_geometry(conn, w2);
            return current && *current == 0 && geometry && !intersects_root(conn, *geometry);
        },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto hidden = property_contains_atom(conn, w2, net_wm_state, net_wm_state_hidden);
            auto state = get_wm_state(conn, w2, wm_state);
            auto client_list = get_client_list(conn, net_client_list);
            auto active = get_window_property_window(conn.get(), conn.root(), net_active_window);
            auto focus = get_input_focus(conn);
            return window_is_viewable(conn, w2) && state && *state == XCB_ICCCM_WM_STATE_NORMAL && hidden && !*hidden
                && client_list && *client_list == *client_list_before && active && *active == w1 && focus && *focus == w1;
        },
        kTimeout
    ));
    synchronize_x_server(conn);
    CHECK_FALSE(drain_window_visibility_events(conn, w2));

    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, w2);
            return geometry && intersects_root(conn, *geometry);
        },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto hidden = property_contains_atom(conn, w2, net_wm_state, net_wm_state_hidden);
            auto state = get_wm_state(conn, w2, wm_state);
            auto client_list = get_client_list(conn, net_client_list);
            auto active = get_window_property_window(conn.get(), conn.root(), net_active_window);
            auto focus = get_input_focus(conn);
            return window_is_viewable(conn, w2) && state && *state == XCB_ICCCM_WM_STATE_NORMAL && hidden && !*hidden
                && client_list && *client_list == *client_list_before && active && *active == w2 && focus && *focus == w2;
        },
        kTimeout
    ));
    synchronize_x_server(conn);
    CHECK_FALSE(drain_window_visibility_events(conn, w2));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

// =============================================================================
// Monocle layout: every tiled window should occupy the same content rect after
// `lwmctl layout set monocle`. Stacking decides which is visible on top.
// =============================================================================
TEST_CASE("Integration: monocle layout assigns identical geometries", "[integration][layout][monocle]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    if (!lwmctl_available())
        SKIP("lwmctl binary not available");

    auto& conn = test_env->conn;
    auto socket_path = wait_for_ipc_socket_path(conn);
    REQUIRE(socket_path.has_value());

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    xcb_window_t w3 = create_window(conn, 70, 70, 200, 150);
    map_window(conn, w3);
    REQUIRE(wait_for_active_window(conn, w3, kTimeout));

    auto g1 = get_window_geometry(conn, w1);
    auto g2 = get_window_geometry(conn, w2);
    auto g3 = get_window_geometry(conn, w3);
    REQUIRE(g1.has_value());
    REQUIRE(g2.has_value());
    REQUIRE(g3.has_value());
    bool all_equal_master_stack = (*g1 == *g2) && (*g2 == *g3);
    REQUIRE_FALSE(all_equal_master_stack);

    auto result = run_lwmctl(test_env->wm, { "layout", "set", "monocle" }, *socket_path);
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);

    bool ok = wait_for_condition(
        [&]()
        {
            auto a = get_window_geometry(conn, w1);
            auto b = get_window_geometry(conn, w2);
            auto c = get_window_geometry(conn, w3);
            return a && b && c && *a == *b && *b == *c;
        },
        kTimeout);
    REQUIRE(ok);

    auto restore = run_lwmctl(test_env->wm, { "layout", "set", "master-stack" }, *socket_path);
    REQUIRE(restore.has_value());
    REQUIRE(restore->exit_code == 0);

    destroy_window(conn, w3);
    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: monocle layout survives exec restart", "[integration][layout][monocle][restart]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    if (!lwmctl_available())
        SKIP("lwmctl binary not available");

    auto& conn = test_env->conn;
    auto socket_path = wait_for_ipc_socket_path(conn);
    REQUIRE(socket_path.has_value());

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    auto set_layout = run_lwmctl(test_env->wm, { "layout", "set", "monocle" }, *socket_path);
    REQUIRE(set_layout.has_value());
    REQUIRE(set_layout->exit_code == 0);
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto first = get_window_geometry(conn, w1);
            auto second = get_window_geometry(conn, w2);
            return first && second && *first == *second;
        },
        kTimeout
    ));

    xcb_atom_t supporting = intern_atom(conn.get(), "_NET_SUPPORTING_WM_CHECK");
    auto old_supporting = get_window_property_window(conn.get(), conn.root(), supporting);
    REQUIRE(old_supporting.has_value());

    auto restart = run_lwmctl(test_env->wm, { "restart" }, *socket_path);
    REQUIRE(restart.has_value());
    REQUIRE(restart->exit_code == 0);
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto current = get_window_property_window(conn.get(), conn.root(), supporting);
            return current && *current != XCB_NONE && *current != *old_supporting;
        },
        std::chrono::seconds(5)
    ));

    REQUIRE(wait_for_condition(
        [&]()
        {
            auto first = get_window_geometry(conn, w1);
            auto second = get_window_geometry(conn, w2);
            return first && second && *first == *second;
        },
        kTimeout
    ));
    auto list = run_lwmctl(test_env->wm, { "workspace", "list" });
    REQUIRE(list.has_value());
    REQUIRE(list->exit_code == 0);
    REQUIRE(list->stdout_text.find("\"layout\":\"monocle\"") != std::string::npos);

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: version 3 restart handoff survives overlay removal", "[integration][restart][upgrade]")
{
    auto& env = X11TestEnvironment::instance();
    if (!env.available())
        SKIP("Test environment not available");
    if (!lwmctl_available())
        SKIP("lwmctl binary not available");

    X11Connection conn;
    if (!conn.ok())
    {
        WARN("Failed to connect to X server.");
        SKIP("Test environment not available");
    }

    xcb_window_t window = create_window(conn, 10, 10, 200, 150);
    map_window(conn, window);

    xcb_atom_t restart_state = intern_atom(conn.get(), "_LWM_RESTART_STATE");
    xcb_atom_t restart_client = intern_atom(conn.get(), "_LWM_RESTART_CLIENT");
    REQUIRE(restart_state != XCB_NONE);
    REQUIRE(restart_client != XCB_NONE);

    // Reproduce the shortest supported payload emitted by an older version-3
    // binary. Its client state starts with the retired overlay flag.
    std::array<uint32_t, 24> client_state{ };
    client_state[0] = 1;
    client_state[2] = 120;
    client_state[3] = 130;
    client_state[4] = 410;
    client_state[5] = 260;
    client_state[23] = 2; // Floating
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        window,
        restart_client,
        XCB_ATOM_CARDINAL,
        32,
        client_state.size(),
        client_state.data()
    );

    std::array<uint32_t, 7> global_state {
        3, // Version
        0, // Focused monitor
        window,
        0, // Showing desktop
        1, // Monitor count
        1, // Current workspace
        0, // Previous workspace
    };
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        conn.root(),
        restart_state,
        XCB_ATOM_CARDINAL,
        32,
        global_state.size(),
        global_state.data()
    );
    xcb_flush(conn.get());

    LwmProcess wm(env.display(), "[workspaces]\ncount = 2\n");
    REQUIRE(wm.running());
    REQUIRE(wait_for_wm_ready(conn, kTimeout));

    REQUIRE(wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, window);
            return geometry && *geometry == WindowGeometry { 120, 130, 410, 260 };
        },
        kTimeout
    ));

    auto workspaces = run_lwmctl(wm, { "workspace", "list" });
    REQUIRE(workspaces.has_value());
    REQUIRE(workspaces->exit_code == 0);
    REQUIRE(workspaces->stdout_text.find("\"current_workspace\":1") != std::string::npos);

    destroy_window(conn, window);
}

TEST_CASE("Integration: monocle swap focuses adjacent tiled window", "[integration][layout][monocle][swap]")
{
    auto test_env = TestEnvironment::create(R"(
[workspaces]
count = 2

[[binds]]
key = "super+j"
swap_next = true
)");
    if (!test_env)
        SKIP("Test environment not available");
    if (!lwmctl_available())
        SKIP("lwmctl binary not available");

    auto& conn = test_env->conn;
    auto socket_path = wait_for_ipc_socket_path(conn);
    REQUIRE(socket_path.has_value());

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    xcb_window_t w3 = create_window(conn, 70, 70, 200, 150);
    map_window(conn, w3);
    REQUIRE(wait_for_active_window(conn, w3, kTimeout));

    xcb_atom_t dialog = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    REQUIRE(dialog != XCB_NONE);
    xcb_window_t floating = create_window(conn, 100, 100, 180, 120);
    set_window_type(conn, floating, dialog);
    map_window(conn, floating);
    REQUIRE(wait_for_active_window(conn, floating, kTimeout));

    auto set_layout = run_lwmctl(test_env->wm, { "layout", "set", "monocle" }, *socket_path);
    REQUIRE(set_layout.has_value());
    REQUIRE(set_layout->exit_code == 0);

    auto focus = run_lwmctl(
        test_env->wm,
        { "focus", "window=" + std::to_string(w1) },
        *socket_path
    );
    REQUIRE(focus.has_value());
    REQUIRE(focus->exit_code == 0);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("j")));
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    destroy_window(conn, floating);
    destroy_window(conn, w3);
    destroy_window(conn, w2);
    destroy_window(conn, w1);
}
