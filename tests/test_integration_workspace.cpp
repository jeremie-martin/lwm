#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <optional>

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

        LwmProcess wm(env.display(), "[workspaces]\ncount = 2\n");
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

    // Window should still be active and fullscreen
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(check_fullscreen());

    // Exit fullscreen
    values[0] = 0;
    send_client_message(conn, w1, net_wm_state, values[0], values[1], values[2], values[3], values[4]);

    // Verify fullscreen state is cleared
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
    "Integration: moving focused window to hidden workspace transfers focus",
    "[integration][workspace][focus]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_number_of_desktops != XCB_NONE);

    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    if (num_desktops < 2)
        SKIP("Need at least 2 desktops");

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // Move w2 to desktop 1 (currently on desktop 0)
    send_client_message(conn, w2, net_wm_desktop, 1);

    // w2 is now on a hidden workspace — focus should fall back to w1
    CHECK(wait_for_active_window(conn, w1, kTimeout));

    // Verify w2 was actually moved
    CHECK(wait_for_property_cardinal(conn.get(), w2, net_wm_desktop, 1, kTimeout));

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

    // Under master-stack, at least two of the three windows must have differing geometries.
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

    // After the switch all three managed windows share the content rect.
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
