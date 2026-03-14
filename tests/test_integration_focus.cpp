#include "x11_test_harness.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>

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

} // namespace

TEST_CASE("Integration: tiled windows take focus in map order", "[integration][focus]")
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

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: focus restores to previous tiled window after destroy", "[integration][focus]")
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

    destroy_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    destroy_window(conn, w1);
}

TEST_CASE("Integration: floating window grabs focus and yields on destroy", "[integration][focus][floating]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

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
    "Integration: fullscreen window keeps zero border width when focus leaves and returns",
    "[integration][focus][fullscreen]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

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

    // Move focus away.
    xcb_window_t w2 = create_window(conn, 60, 60, 320, 180);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // Request focus back to fullscreen window.
    send_client_message(conn, w1, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // Fullscreen and zero-border constraints should still hold after focus return.
    REQUIRE(wait_for_condition(has_fullscreen_state, kTimeout));
    REQUIRE(wait_for_condition(border_width_is_zero, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: second fullscreen window clears previous fullscreen state on the same workspace",
    "[integration][focus][fullscreen][exclusive]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

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
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    send_client_message(conn, w2, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);

    REQUIRE(wait_for_condition([&]() { return has_fullscreen_state(w2); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !has_fullscreen_state(w1); }, kTimeout));

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
        return;

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
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

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
        return;

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
    "Integration: fullscreen suppression stays scoped to the visible workspace",
    "[integration][focus][fullscreen][workspace]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

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
        return;

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

TEST_CASE(
    "Integration: iconify and deiconify re-arbitrate fullscreen owner",
    "[integration][focus][fullscreen][iconify]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        return;

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
        return;

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
        return;

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
