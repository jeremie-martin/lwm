#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

struct TestEnvironment
{
    X11TestEnvironment& x11_env;
    X11Connection conn;
    LwmProcess wm;

    bool ok() const { return conn.ok() && wm.running(); }

    static std::optional<TestEnvironment> create(std::string const& config)
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

std::string overlay_config()
{
    return R"(
[workspaces]
count = 2
names = ["1", "2"]

[[rules]]
type = "utility"
layer = "overlay"
)";
}

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

bool matches_root_geometry(X11Connection& conn, xcb_window_t window)
{
    auto cookie = xcb_get_geometry(conn.get(), window);
    auto* reply = xcb_get_geometry_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return false;

    bool match = reply->x == 0 && reply->y == 0 && reply->width == conn.screen()->width_in_pixels
        && reply->height == conn.screen()->height_in_pixels && reply->border_width == 0;
    free(reply);
    return match;
}

} // namespace

TEST_CASE("Integration: overlay layer stays above fullscreen windows and remains borderless", "[integration][overlay]")
{
    auto test_env = TestEnvironment::create(overlay_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_wm_window_type_utility = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_UTILITY");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE || net_wm_window_type_utility == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    xcb_window_t fullscreen_window = create_window(conn, 10, 10, 640, 360);
    map_window(conn, fullscreen_window);
    REQUIRE(wait_for_active_window(conn, fullscreen_window, kTimeout));

    send_client_message(conn, fullscreen_window, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition(
        [&]() { return has_state(conn, fullscreen_window, net_wm_state_fullscreen); },
        kTimeout
    ));

    xcb_window_t overlay_window = create_window(conn, 20, 20, 200, 120);
    set_window_type(conn, overlay_window, net_wm_window_type_utility);
    map_window(conn, overlay_window);

    REQUIRE(wait_for_condition([&]() { return matches_root_geometry(conn, overlay_window); }, kTimeout));
    REQUIRE(wait_for_condition(
        [&]() { return !has_state(conn, overlay_window, net_wm_state_fullscreen); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay_window, fullscreen_window); },
        kTimeout
    ));

    xcb_window_t tiled_window = create_window(conn, 60, 60, 320, 200);
    map_window(conn, tiled_window);
    REQUIRE(wait_for_active_window(conn, tiled_window, kTimeout));

    REQUIRE(wait_for_condition([&]() { return matches_root_geometry(conn, overlay_window); }, kTimeout));
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay_window, fullscreen_window); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay_window, tiled_window); },
        kTimeout
    ));

    destroy_window(conn, tiled_window);
    destroy_window(conn, overlay_window);
    destroy_window(conn, fullscreen_window);
}

TEST_CASE("Integration: overlay remains above after fullscreen owner changes", "[integration][overlay]")
{
    auto test_env = TestEnvironment::create(overlay_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_wm_window_type_utility = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_UTILITY");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE || net_wm_window_type_utility == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    // Create two tiled windows.
    xcb_window_t tiled1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, tiled1);
    REQUIRE(wait_for_active_window(conn, tiled1, kTimeout));

    xcb_window_t tiled2 = create_window(conn, 20, 20, 640, 360);
    map_window(conn, tiled2);
    REQUIRE(wait_for_active_window(conn, tiled2, kTimeout));

    // Create an overlay window.
    xcb_window_t overlay = create_window(conn, 30, 30, 200, 120);
    set_window_type(conn, overlay, net_wm_window_type_utility);
    map_window(conn, overlay);
    REQUIRE(wait_for_condition([&]() { return matches_root_geometry(conn, overlay); }, kTimeout));

    // Fullscreen the first tiled window.
    send_client_message(conn, tiled1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition(
        [&]() { return has_state(conn, tiled1, net_wm_state_fullscreen); },
        kTimeout
    ));

    // Overlay must be above the fullscreen window.
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay, tiled1); },
        kTimeout
    ));

    // Fullscreen the second tiled window (strips fullscreen from the first).
    send_client_message(conn, tiled2, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition(
        [&]() { return has_state(conn, tiled2, net_wm_state_fullscreen); },
        kTimeout
    ));

    // Overlay must still be above the new fullscreen window.
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay, tiled2); },
        kTimeout
    ));

    destroy_window(conn, overlay);
    destroy_window(conn, tiled2);
    destroy_window(conn, tiled1);
}

TEST_CASE("Integration: overlay stays above normal tiled windows without fullscreen", "[integration][overlay]")
{
    auto test_env = TestEnvironment::create(overlay_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_window_type_utility = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_UTILITY");
    if (net_wm_window_type_utility == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    // Create several tiled windows.
    xcb_window_t tiled1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, tiled1);
    REQUIRE(wait_for_active_window(conn, tiled1, kTimeout));

    xcb_window_t tiled2 = create_window(conn, 20, 20, 640, 360);
    map_window(conn, tiled2);
    REQUIRE(wait_for_active_window(conn, tiled2, kTimeout));

    xcb_window_t tiled3 = create_window(conn, 30, 30, 640, 360);
    map_window(conn, tiled3);
    REQUIRE(wait_for_active_window(conn, tiled3, kTimeout));

    // Create an overlay window.
    xcb_window_t overlay = create_window(conn, 40, 40, 200, 120);
    set_window_type(conn, overlay, net_wm_window_type_utility);
    map_window(conn, overlay);

    // Overlay must cover the root geometry.
    REQUIRE(wait_for_condition([&]() { return matches_root_geometry(conn, overlay); }, kTimeout));

    // Overlay must be stacked above all tiled windows.
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay, tiled1); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay, tiled2); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay, tiled3); },
        kTimeout
    ));

    destroy_window(conn, overlay);
    destroy_window(conn, tiled3);
    destroy_window(conn, tiled2);
    destroy_window(conn, tiled1);
}

TEST_CASE("Integration: overlay is sticky and survives workspace switch", "[integration][overlay]")
{
    auto test_env = TestEnvironment::create(overlay_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_sticky = intern_atom(conn.get(), "_NET_WM_STATE_STICKY");
    xcb_atom_t net_wm_window_type_utility = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_UTILITY");
    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    if (net_wm_state == XCB_NONE || net_wm_state_sticky == XCB_NONE
        || net_wm_window_type_utility == XCB_NONE || net_current_desktop == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    // Create an overlay (utility) window on workspace 0.
    xcb_window_t overlay = create_window(conn, 20, 20, 200, 120);
    set_window_type(conn, overlay, net_wm_window_type_utility);
    map_window(conn, overlay);
    REQUIRE(wait_for_condition([&]() { return matches_root_geometry(conn, overlay); }, kTimeout));

    // Overlay must have _NET_WM_STATE_STICKY set.
    REQUIRE(wait_for_condition(
        [&]() { return has_state(conn, overlay, net_wm_state_sticky); },
        kTimeout
    ));

    // Switch to workspace 1.
    send_client_message(conn, conn.root(), net_current_desktop, 1, 0, 0, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Overlay must still have root geometry (visible, not hidden off-screen).
    REQUIRE(wait_for_condition([&]() { return matches_root_geometry(conn, overlay); }, kTimeout));

    // Create a tiled window on workspace 1.
    xcb_window_t tiled = create_window(conn, 10, 10, 640, 360);
    map_window(conn, tiled);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    // Overlay must still be above the tiled window.
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay, tiled); },
        kTimeout
    ));

    destroy_window(conn, tiled);
    destroy_window(conn, overlay);
}

TEST_CASE("Integration: multiple overlay windows both stay above fullscreen", "[integration][overlay]")
{
    auto test_env = TestEnvironment::create(overlay_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_wm_window_type_utility = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_UTILITY");
    if (net_wm_state == XCB_NONE || net_wm_state_fullscreen == XCB_NONE || net_wm_window_type_utility == XCB_NONE)
    {
        WARN("Failed to intern EWMH atoms.");
        return;
    }

    // Create a window and make it fullscreen.
    xcb_window_t fullscreen_window = create_window(conn, 10, 10, 640, 360);
    map_window(conn, fullscreen_window);
    REQUIRE(wait_for_active_window(conn, fullscreen_window, kTimeout));

    send_client_message(conn, fullscreen_window, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition(
        [&]() { return has_state(conn, fullscreen_window, net_wm_state_fullscreen); },
        kTimeout
    ));

    // Create two overlay (utility) windows.
    xcb_window_t overlay1 = create_window(conn, 20, 20, 200, 120);
    set_window_type(conn, overlay1, net_wm_window_type_utility);
    map_window(conn, overlay1);
    REQUIRE(wait_for_condition([&]() { return matches_root_geometry(conn, overlay1); }, kTimeout));

    xcb_window_t overlay2 = create_window(conn, 30, 30, 200, 120);
    set_window_type(conn, overlay2, net_wm_window_type_utility);
    map_window(conn, overlay2);
    REQUIRE(wait_for_condition([&]() { return matches_root_geometry(conn, overlay2); }, kTimeout));

    // Both overlays must be stacked above the fullscreen window.
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay1, fullscreen_window); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return is_stacked_above(conn, overlay2, fullscreen_window); },
        kTimeout
    ));

    destroy_window(conn, overlay2);
    destroy_window(conn, overlay1);
    destroy_window(conn, fullscreen_window);
}
