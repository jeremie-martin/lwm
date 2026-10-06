#include "wm_observations.hpp"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <xcb/randr.h>
#include <xcb/xtest.h>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

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

void set_wm_hints_urgency(X11Connection& conn, xcb_window_t window)
{
    constexpr uint32_t XUrgencyHint = 256;
    xcb_icccm_wm_hints_t hints = {};
    hints.flags = XCB_ICCCM_WM_HINT_INPUT | XUrgencyHint;
    hints.input = 1;
    xcb_icccm_set_wm_hints(conn.get(), window, &hints);
    xcb_flush(conn.get());
}

std::string floating_hidden_workspace_rule_config()
{
    return R"(
[workspaces]
names = ["one", "two"]

[[rules]]
match = { class = "FloatHidden" }
apply = { floating = true, workspace = 1 }
)";
}

bool set_randr_screen_size(X11Connection& conn, uint16_t width, uint16_t height)
{
    uint32_t const width_mm = std::max<uint16_t>(conn.screen()->width_in_millimeters, 1);
    uint32_t const height_mm = std::max<uint16_t>(conn.screen()->height_in_millimeters, 1);
    auto cookie = xcb_randr_set_screen_size_checked(conn.get(), conn.root(), width, height, width_mm, height_mm);
    auto* error = xcb_request_check(conn.get(), cookie);
    if (error)
    {
        free(error);
        return false;
    }

    xcb_flush(conn.get());
    return wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, conn.root());
            return geometry && geometry->width == width && geometry->height == height;
        },
        kTimeout
    );
}

struct RandrScreenSizeGuard
{
    X11Connection& conn;
    uint16_t width = 0;
    uint16_t height = 0;
    bool restore = false;

    ~RandrScreenSizeGuard()
    {
        if (restore)
            (void)set_randr_screen_size(conn, width, height);
    }
};

} // namespace

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
    REQUIRE(net_active_window != XCB_NONE);

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
    REQUIRE(net_restack_window != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w2, w1); }, kTimeout));

    send_client_message(conn, w1, net_restack_window, 2, w2, XCB_STACK_MODE_ABOVE, 0, 0);
    observe_title_after_events(conn, w1);

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
    REQUIRE(dialog_type != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

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
    "Integration: tiled window mapped after a floating dialog stays below it",
    "[integration][focus][stacking][regression]"
)
{
    // Regression test for the floating-behind-tile bug:
    // when a fresh tiled window is mapped while a floating dialog already
    // exists, the X server places the new window at the top of the stack by
    // default.  The WM must restack so the dialog ends up above the new tile.
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t dialog_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    REQUIRE(dialog_type != XCB_NONE);

    xcb_window_t parent_tile = create_window(conn, 10, 10, 200, 150);
    map_window(conn, parent_tile);
    REQUIRE(wait_for_active_window(conn, parent_tile, kTimeout));

    xcb_window_t floating = create_window(conn, 60, 60, 180, 120);
    set_window_type(conn, floating, dialog_type);
    set_transient_for(conn, floating, parent_tile);
    map_window(conn, floating);
    REQUIRE(wait_for_active_window(conn, floating, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, floating, parent_tile); }, kTimeout));

    // Map a second tile.  Without global stacking, this freshly mapped X
    // window would sit at the top of the stack and obscure the floating
    // dialog.
    xcb_window_t intruder_tile = create_window(conn, 30, 30, 220, 170);
    map_window(conn, intruder_tile);
    REQUIRE(wait_for_active_window(conn, intruder_tile, kTimeout));

    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, floating, intruder_tile); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, floating, parent_tile); }, kTimeout));

    destroy_window(conn, intruder_tile);
    destroy_window(conn, floating);
    destroy_window(conn, parent_tile);
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
    REQUIRE(dialog_type != XCB_NONE);
    REQUIRE(net_restack_window != XCB_NONE);

    xcb_window_t tiled = create_window(conn, 10, 10, 200, 150);
    map_window(conn, tiled);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    xcb_window_t floating = create_window(conn, 60, 60, 180, 120);
    set_window_type(conn, floating, dialog_type);
    map_window(conn, floating);
    REQUIRE(wait_for_active_window(conn, floating, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, floating, tiled); }, kTimeout));

    send_client_message(conn, tiled, net_restack_window, 2, floating, XCB_STACK_MODE_ABOVE, 0, 0);
    observe_title_after_events(conn, tiled);

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
    xcb_atom_t lwm_window_class = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    REQUIRE(dialog_type != XCB_NONE);
    REQUIRE(lwm_window_class != XCB_NONE);

    xcb_window_t floating = create_window(conn, 60, 60, 180, 120);
    REQUIRE(set_window_type(conn, floating, dialog_type));
    map_window(conn, floating);
    REQUIRE(wait_for_active_window(conn, floating, kTimeout));
    REQUIRE(wait_for_property_strings(conn.get(), floating, lwm_window_class, { "floating" }, kTimeout));

    destroy_window(conn, floating);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    destroy_window(conn, tiled);
}

TEST_CASE(
    "Integration: floating workspace rule does not focus a hidden mapped window",
    "[integration][focus][floating][rules][workspace]"
)
{
    auto test_env = TestEnvironment::create(floating_hidden_workspace_rule_config());
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

    xcb_window_t floating = create_window(conn, 60, 60, 220, 160);
    set_window_wm_class(conn, floating, "float-hidden", "FloatHidden");
    map_window(conn, floating);

    REQUIRE(wait_for_property_cardinal(conn.get(), floating, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, floating); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    destroy_window(conn, floating);
    destroy_window(conn, fallback);
}

TEST_CASE(
    "Integration: hidden floating configure request stays off-screen until shown",
    "[integration][focus][floating][rules][workspace][configure]"
)
{
    auto test_env = TestEnvironment::create(floating_hidden_workspace_rule_config());
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

    xcb_window_t floating = create_window(conn, 60, 60, 220, 160);
    set_window_wm_class(conn, floating, "float-hidden", "FloatHidden");
    map_window(conn, floating);

    REQUIRE(wait_for_property_cardinal(conn.get(), floating, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, floating); }, kTimeout));

    uint32_t values[] = { 100, 110, 260, 180 };
    xcb_configure_window(
        conn.get(),
        floating,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        values
    );
    xcb_flush(conn.get());

    observe_title_after_events(conn, floating);
    REQUIRE(is_hidden_offscreen(conn, floating));
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, floating);
            return geometry.has_value() && geometry->x == 100 && geometry->y == 110 && geometry->width == 260
                && geometry->height == 180;
        },
        kTimeout
    ));

    destroy_window(conn, floating);
    destroy_window(conn, fallback);
}

TEST_CASE(
    "Integration: focused initially urgent floating window clears urgency",
    "[integration][focus][floating][urgent]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t dialog_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(dialog_type != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(demands_attention != XCB_NONE);

    xcb_window_t tiled = create_window(conn, 10, 10, 220, 160);
    map_window(conn, tiled);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    xcb_window_t floating = create_window(conn, 60, 60, 220, 160);
    set_window_type(conn, floating, dialog_type);
    set_wm_hints_urgency(conn, floating);
    map_window(conn, floating);

    REQUIRE(wait_for_active_window(conn, floating, kTimeout));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), floating, net_wm_state, demands_attention); },
        kTimeout
    ));

    destroy_window(conn, floating);
    destroy_window(conn, tiled);
}

TEST_CASE("Integration: a Super+Button2 command binding focuses and floats the clicked window", "[integration][focus][mouse]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    if (!extension_available(conn, &xcb_test_id))
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
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // Add fullscreen state.
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);

    auto has_fullscreen_state = [&] { return has_state(conn, w1, net_wm_state_fullscreen); };

    auto border_width_is_zero = [&] { return get_window_border_width(conn, w1) == 0; };

    REQUIRE(wait_for_condition(has_fullscreen_state, kTimeout));
    REQUIRE(wait_for_condition(border_width_is_zero, kTimeout));

    // A transient may take focus above its fullscreen parent.
    xcb_window_t w2 = create_window(conn, 60, 60, 320, 180);
    REQUIRE(set_window_type(conn, w2, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG")));
    xcb_icccm_set_wm_transient_for(conn.get(), w2, w1);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_x_input_focus(conn, w2, kTimeout));
    REQUIRE(border_width_is_zero());
    send_client_message(conn, w1, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_x_input_focus(conn, w1, kTimeout));
    REQUIRE(has_fullscreen_state());
    REQUIRE(border_width_is_zero());

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
    xcb_atom_t dialog_type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);
    REQUIRE(dialog_type != XCB_NONE);

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

    // Floating fullscreen geometry is applied immediately, not only after hide/show.
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

    auto previous_wm = wm_instance(conn);
    REQUIRE(previous_wm.has_value());
    auto restart_result = run_lwmctl(wm, { "restart" });
    (void)restart_result;
    REQUIRE(wait_for_wm_restart(conn, std::chrono::seconds(5), *previous_wm));

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
    "Integration: fullscreen round-trip preserves below layer request",
    "[integration][focus][fullscreen][stacking]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_wm_state_below = intern_atom(conn.get(), "_NET_WM_STATE_BELOW");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);
    REQUIRE(net_wm_state_below != XCB_NONE);

    xcb_window_t window = create_window(conn, 10, 10, 360, 240);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    send_client_message(conn, window, net_wm_state, 1, net_wm_state_below, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, window, net_wm_state_below); }, kTimeout));

    send_client_message(conn, window, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, window, net_wm_state_fullscreen); }, kTimeout));

    send_client_message(conn, window, net_wm_state, 0, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, window, net_wm_state_fullscreen); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, window, net_wm_state_below); }, kTimeout));

    destroy_window(conn, window);
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
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);

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
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

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
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);
    REQUIRE(net_wm_state_sticky != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

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

TEST_CASE("Integration: lwmctl focus reports redirected focus as failure", "[integration][focus][fullscreen][ipc]")
{
    auto test_env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_wm_state_sticky = intern_atom(conn.get(), "_NET_WM_STATE_STICKY");
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);
    REQUIRE(net_wm_state_sticky != XCB_NONE);

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

    auto result = run_lwmctl(test_env->wm, { "window", "focus", std::to_string(target) });
    REQUIRE(result);

    REQUIRE(result->exit_code != 0);
    REQUIRE(result->stderr_text.find("focus request refused") != std::string::npos);
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
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

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
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 180);
    REQUIRE(set_window_type(conn, w2, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG")));
    map_window(conn, w2);
    REQUIRE(wait_for_condition(
        [&]
        {
            return get_window_property_string(conn.get(), w2, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"))
                == "floating";
        },
        kTimeout
    ));
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_stacked_above(conn, w1, w2); }, kTimeout));

    uint32_t values[] = { w1, XCB_STACK_MODE_ABOVE };
    xcb_configure_window(conn.get(), w2, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
    xcb_flush(conn.get());
    observe_title_after_events(conn, w1);

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
    REQUIRE(net_active_window != XCB_NONE);
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_wm_state_sticky != XCB_NONE);

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

TEST_CASE("Integration: stashing a visible sticky window restores focus fallback", "[integration][focus][sticky]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state_hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    xcb_atom_t net_wm_state_sticky = intern_atom(conn.get(), "_NET_WM_STATE_STICKY");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_wm_state_hidden != XCB_NONE);
    REQUIRE(net_wm_state_sticky != XCB_NONE);

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

    ipc_ok("scratchpad stash");

    REQUIRE(wait_for_condition([&]() { return has_state(conn, sticky, net_wm_state_hidden); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, fallback, kTimeout));

    destroy_window(conn, sticky);
    destroy_window(conn, fallback);
}

TEST_CASE(
    "Integration: removing sticky from off-workspace active window restores focus fallback",
    "[integration][focus][sticky]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state_sticky = intern_atom(conn.get(), "_NET_WM_STATE_STICKY");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_wm_state_sticky != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

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
    send_client_message(conn, sticky, net_active_window, 2, XCB_CURRENT_TIME, 0, 0, 0);
    REQUIRE(wait_for_active_window(conn, sticky, kTimeout));

    send_client_message(conn, sticky, net_wm_state, 0, net_wm_state_sticky, 0, 0, 0);

    REQUIRE(wait_for_condition([&]() { return !has_state(conn, sticky, net_wm_state_sticky); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, sticky); }, kTimeout));
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
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);

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
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);
    REQUIRE(net_restack_window != XCB_NONE);

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
    observe_title_after_events(conn, w1);

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
    REQUIRE(dialog_type != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);

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
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);

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
    REQUIRE(dialog_type != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);

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

TEST_CASE("Integration: application minimize requests are ignored", "[integration][focus][fullscreen][iconify]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_wm_state_hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    // Neither _NET_WM_STATE_HIDDEN nor ICCCM WM_CHANGE_STATE hides a window.
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_hidden, 0, 0, 0);
    send_client_message(conn, w1, intern_atom(conn.get(), "WM_CHANGE_STATE"), XCB_ICCCM_WM_STATE_ICONIC);
    observe_title_after_events(conn, w1);
    CHECK_FALSE(has_state(conn, w1, net_wm_state_hidden));
    CHECK_FALSE(is_hidden_offscreen(conn, w1));
    CHECK(wait_for_active_window(conn, w1, kTimeout));
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: RandR rebuild restores fullscreen owner and suppression",
    "[integration][focus][fullscreen][randr]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    if (!test_env->x11_env.owns_display())
        SKIP("RandR screen-size mutation is only safe on the owned Xvfb display.");

    auto& conn = test_env->conn;
    if (!extension_available(conn, &xcb_randr_id))
        SKIP("RandR extension not available.");

    auto original_geometry = get_window_geometry(conn, conn.root());
    if (!original_geometry || original_geometry->width <= 1)
        SKIP("Root window geometry is not suitable for RandR resize test.");

    RandrScreenSizeGuard screen_guard{
        .conn = conn,
        .width = original_geometry->width,
        .height = original_geometry->height,
    };

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 640, 360);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    send_client_message(conn, w1, net_wm_state, 1, net_wm_state_fullscreen, 0, 0, 0);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_fullscreen); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 180);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, w2); }, kTimeout));

    uint16_t const target_width = static_cast<uint16_t>(original_geometry->width - 1);
    if (!set_randr_screen_size(conn, target_width, original_geometry->height))
        SKIP("RandR screen-size change not supported by this X server.");
    screen_guard.restore = true;
    REQUIRE(wait_for_condition(
        [&]
        {
            auto geometry = get_window_geometry(conn, w1);
            return geometry && geometry->width == target_width && geometry->height == original_geometry->height;
        },
        kTimeout
    ));

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            return has_state(conn, w1, net_wm_state_fullscreen) && !is_hidden_offscreen(conn, w1)
                && is_hidden_offscreen(conn, w2);
        },
        kTimeout
    ));

    REQUIRE(set_randr_screen_size(conn, screen_guard.width, screen_guard.height));
    screen_guard.restore = false;

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: a focus change clears _NET_WM_STATE_FOCUSED from the previous window", "[integration][focus][ewmh]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_focused = intern_atom(conn.get(), "_NET_WM_STATE_FOCUSED");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_focused != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 20, 20, 320, 200);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, net_wm_state_focused); }, kTimeout));

    xcb_window_t w2 = create_window(conn, 80, 80, 320, 200);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w1, net_wm_state_focused); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w2, net_wm_state_focused); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
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
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_number_of_desktops != XCB_NONE);

    uint32_t num_desktops = require_property_cardinal(conn.get(), conn.root(), net_number_of_desktops);
    REQUIRE(num_desktops >= 2);

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

TEST_CASE("Integration: multiple transients of same parent all stack above it", "[integration][transient][stacking]")
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

TEST_CASE("Integration: unchanged stacking policy repairs an external server restack", "[integration][stacking]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    xcb_warp_pointer(conn.get(), XCB_NONE, conn.root(), 0, 0, 0, 0, 0, 0);
    xcb_flush(conn.get());
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 6; ++i)
    {
        auto window = create_window(conn, 10, 10, 100, 100);
        windows.push_back(window);
        map_window(conn, window);
        REQUIRE(wait_for_active_window(conn, window, kTimeout));
    }
    auto actual_order = [&]
    {
        auto* reply = xcb_query_tree_reply(conn.get(), xcb_query_tree(conn.get(), conn.root()), nullptr);
        std::vector<xcb_window_t> result;
        if (reply)
        {
            auto* children = xcb_query_tree_children(reply);
            for (int i = 0; i < xcb_query_tree_children_length(reply); ++i)
                if (std::ranges::find(windows, children[i]) != windows.end())
                    result.push_back(children[i]);
        }
        free(reply);
        return result;
    };
    REQUIRE(wait_for_condition([&] { return actual_order() == windows; }, kTimeout));
    // Bypass SubstructureRedirect to mutate server order outside the WM funnel.
    uint32_t override_redirect = 1, above = XCB_STACK_MODE_ABOVE;
    xcb_change_window_attributes(conn.get(), windows.front(), XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
    xcb_configure_window(conn.get(), windows.front(), XCB_CONFIG_WINDOW_STACK_MODE, &above);
    override_redirect = 0;
    xcb_change_window_attributes(conn.get(), windows.front(), XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
    xcb_flush(conn.get());
    REQUIRE(wait_for_condition(
        [&]
        {
            auto order = actual_order();
            return !order.empty() && order.back() == windows.front();
        },
        kTimeout
    ));
    auto result = run_lwmctl(env->wm, { "window", "focus", std::to_string(windows.back()) });
    REQUIRE(result);
    REQUIRE(result->exit_code == 0);
    REQUIRE(wait_for_condition([&] { return actual_order() == windows; }, kTimeout));
    for (auto window : windows) destroy_window(conn, window);
}

TEST_CASE(
    "Integration: focus preserves unknown state atoms and reasserts unchanged focus",
    "[integration][focus][ewmh]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto a = create_window(conn, 10, 10, 100, 100);
    auto b = create_window(conn, 10, 10, 100, 100);
    map_window(conn, a);
    REQUIRE(wait_for_active_window(conn, a, kTimeout));
    map_window(conn, b);
    REQUIRE(wait_for_active_window(conn, b, kTimeout));
    auto unknown = intern_atom(conn.get(), "_LWM_TEST_UNKNOWN_STATE");
    auto focused = intern_atom(conn.get(), "_NET_WM_STATE_FOCUSED");
    set_initial_window_state(conn, a, { unknown });
    set_initial_window_state(conn, b, { unknown, focused });
    for (auto target : { a, b })
    {
        auto result = run_lwmctl(env->wm, { "window", "focus", std::to_string(target) });
        REQUIRE(result);
        REQUIRE(result->exit_code == 0);
        CHECK(has_state(conn, target, focused));
        CHECK_FALSE(has_state(conn, target == a ? b : a, focused));
        CHECK(has_state(conn, a, unknown));
        CHECK(has_state(conn, b, unknown));
    }
    set_initial_window_state(conn, b, { unknown });
    xcb_set_input_focus(conn.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn.root(), XCB_CURRENT_TIME);
    xcb_flush(conn.get());
    REQUIRE(wait_for_x_input_focus(conn, conn.root(), kTimeout));
    auto result = run_lwmctl(env->wm, { "window", "focus", std::to_string(b) });
    REQUIRE(result);
    REQUIRE(result->exit_code == 0);
    REQUIRE(wait_for_x_input_focus(conn, b, kTimeout));
    CHECK(has_state(conn, b, focused));
    CHECK(has_state(conn, b, unknown));
    destroy_window(conn, a);
    destroy_window(conn, b);
}

TEST_CASE(
    "Integration: consecutive focus cycling walks stable MRU and resets after activation",
    "[integration][focus][cycle]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    xcb_warp_pointer(conn.get(), XCB_NONE, conn.root(), 0, 0, 0, 0, 0, 0);
    xcb_flush(conn.get());
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 4; ++i)
    {
        auto w = create_window(conn, 20, 20, 200, 150);
        if (i == 1)
            set_transient_for(conn, w, windows.front());
        windows.push_back(w);
        map_window(conn, w);
        REQUIRE(wait_for_active_window(conn, w, kTimeout));
    }
    auto command = [&](std::string argument, xcb_window_t expected)
    {
        auto result = run_lwmctl(env->wm, { "window", "focus", argument });
        REQUIRE(result);
        REQUIRE(result->exit_code == 0);
        REQUIRE(result->stdout_text.empty());
        REQUIRE(wait_for_active_window(conn, expected, kTimeout));
        REQUIRE(wait_for_x_input_focus(conn, expected, kTimeout));
    };
    // Establish recency independently of mapping/layout order: 0, 2, 1, 3.
    for (int i : { 3, 1, 2, 0 }) command(std::to_string(windows[i]), windows[i]);
    for (int i : { 2, 1, 3, 0, 2 }) command("next", windows[i]);
    for (int i : { 0, 3, 1, 2 }) command("prev", windows[i]);
    // Explicit same-window activation also starts a fresh MRU traversal.
    command(std::to_string(windows[2]), windows[2]);
    command("next", windows[1]);
    command("next", windows[3]);
    // Destruction of an inactive member must not leave a stale target.
    destroy_window(conn, windows[0]);
    observe_title_after_events(conn, windows[3]);
    command("next", windows[2]);
    auto added = create_window(conn, 20, 20, 200, 150);
    map_window(conn, added);
    REQUIRE(wait_for_active_window(conn, added, kTimeout));
    command("next", windows[2]);
    command("next", windows[3]);
    for (int i : { 1, 2, 3 }) destroy_window(conn, windows[i]);
    destroy_window(conn, added);
}

TEST_CASE(
    "Integration: keyboard cycling includes sticky tiles and observes changed input hints",
    "[integration][focus][cycle]"
)
{
    auto env = TestEnvironment::create(R"(
[workspaces]
names = ["1", "2"]
[binds]
"F5" = "window focus next"
"F6" = "window focus prev"
)");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    xcb_warp_pointer(conn.get(), XCB_NONE, conn.root(), 0, 0, 0, 0, 0, 0);
    xcb_flush(conn.get());
    auto sticky = create_window(conn, 20, 20, 200, 150);
    set_initial_window_state(conn, sticky, { intern_atom(conn.get(), "_NET_WM_STATE_STICKY") });
    map_window(conn, sticky);
    REQUIRE(wait_for_active_window(conn, sticky, kTimeout));
    send_client_message(conn, conn.root(), intern_atom(conn.get(), "_NET_CURRENT_DESKTOP"), 1);
    REQUIRE(wait_for_property_cardinal(
        conn.get(),
        conn.root(),
        intern_atom(conn.get(), "_NET_CURRENT_DESKTOP"),
        1,
        kTimeout
    ));
    auto a = create_window(conn, 20, 20, 200, 150);
    map_window(conn, a);
    REQUIRE(wait_for_active_window(conn, a, kTimeout));
    auto b = create_window(conn, 20, 20, 200, 150);
    map_window(conn, b);
    REQUIRE(wait_for_active_window(conn, b, kTimeout));
    auto key = [&](xcb_keysym_t symbol, xcb_window_t expected)
    {
        auto code = first_keycode_for_keysym(conn, symbol);
        REQUIRE(code);
        xcb_test_fake_input(conn.get(), XCB_KEY_PRESS, *code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
        xcb_test_fake_input(conn.get(), XCB_KEY_RELEASE, *code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
        xcb_flush(conn.get());
        observe_title_after_events(conn, b);
        REQUIRE(wait_for_active_window(conn, expected, kTimeout));
    };
    key(XK_F5, a);
    key(XK_F5, sticky);
    key(XK_F6, a);
    xcb_icccm_wm_hints_t hints{ };
    hints.flags = XCB_ICCCM_WM_HINT_INPUT;
    hints.input = 0;
    xcb_icccm_set_wm_hints(conn.get(), sticky, &hints);
    observe_title_after_events(conn, b);
    key(XK_F5, b);
    hints.input = 1;
    xcb_icccm_set_wm_hints(conn.get(), sticky, &hints);
    observe_title_after_events(conn, b);
    key(XK_F6, sticky);
    for (auto window : { sticky, a, b }) destroy_window(conn, window);
}

TEST_CASE(
    "Integration: transient chains and cycles have stable server and published order",
    "[integration][stacking][transient][stack_order]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    xcb_warp_pointer(conn.get(), XCB_NONE, conn.root(), 0, 0, 0, 0, 0, 0);
    xcb_flush(conn.get());
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 3; ++i)
    {
        auto w = create_window(conn, 20, 20, 200, 150);
        set_window_type(conn, w, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
        map_window(conn, w);
        windows.push_back(w);
        REQUIRE(wait_for_active_window(conn, w, kTimeout));
    }
    auto [a, b, c] = std::tuple{ windows[0], windows[1], windows[2] };
    auto require_order = [&](std::vector<xcb_window_t> const& expected)
    {
        observe_title_after_events(conn, c);
        REQUIRE(wait_for_condition(
            [&]
            {
                auto published = get_window_property_windows(
                    conn.get(),
                    conn.root(),
                    intern_atom(conn.get(), "_NET_CLIENT_LIST_STACKING")
                );
                if (published != expected)
                    return false;
                auto* reply = xcb_query_tree_reply(conn.get(), xcb_query_tree(conn.get(), conn.root()), nullptr);
                if (!reply)
                    return false;
                std::vector<xcb_window_t> actual;
                auto* children = xcb_query_tree_children(reply);
                for (int i = 0; i < xcb_query_tree_children_length(reply); ++i)
                    if (std::ranges::find(windows, children[i]) != windows.end())
                        actual.push_back(children[i]);
                free(reply);
                return actual == expected;
            },
            kTimeout
        ));
    };
    set_transient_for(conn, a, b);
    set_transient_for(conn, b, c);
    require_order({ c, b, a });
    set_transient_for(conn, c, a);
    // a is the lowest base-ranked cycle member; ignore only its parent edge.
    require_order({ a, c, b });
    for (int i = 0; i < 3; ++i)
    {
        auto reply = run_lwmctl(env->wm, { "window", "focus", std::to_string(c) });
        REQUIRE(reply);
        REQUIRE(reply->exit_code == 0);
        require_order({ a, c, b });
    }
    set_transient_for(conn, b, b);
    require_order({ b, a, c });
    set_transient_for(conn, b, 0x7fffffff);
    require_order({ b, a, c });
    set_transient_for(conn, c, b);
    for (auto target : { a, c })
    {
        auto reply = run_lwmctl(env->wm, { "window", "focus", std::to_string(target) });
        REQUIRE(reply);
        REQUIRE(reply->exit_code == 0);
        require_order(target == a ? std::vector<xcb_window_t>{ b, c, a } : std::vector<xcb_window_t>{ b, a, c });
    }
    set_transient_for(conn, c, a);
    require_order({ b, a, c });
    // Removing an actual parent releases its child's ordering constraint.
    destroy_window(conn, b);
    windows = { a, c };
    require_order({ a, c });
    destroy_window(conn, a);
    destroy_window(conn, c);
}

TEST_CASE(
    "Integration: fullscreen-suppressed clients precede visible desktops in published order",
    "[integration][stacking][fullscreen][stack_order]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto desktop = create_window(conn, 0, 0, 100, 100);
    set_window_type(conn, desktop, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DESKTOP"));
    map_window(conn, desktop);
    auto suppressed = create_window(conn, 10, 10, 200, 150);
    map_window(conn, suppressed);
    REQUIRE(wait_for_active_window(conn, suppressed, kTimeout));
    auto owner = create_window(conn, 20, 20, 200, 150);
    map_window(conn, owner);
    REQUIRE(wait_for_active_window(conn, owner, kTimeout));
    send_client_message(
        conn,
        owner,
        intern_atom(conn.get(), "_NET_WM_STATE"),
        1,
        intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN")
    );
    observe_title_after_events(conn, owner);
    REQUIRE(is_hidden_offscreen(conn, suppressed));
    auto order =
        get_window_property_windows(conn.get(), conn.root(), intern_atom(conn.get(), "_NET_CLIENT_LIST_STACKING"));
    REQUIRE(order == std::vector<xcb_window_t>{ suppressed, desktop, owner });
    REQUIRE(is_stacked_above(conn, owner, desktop));
    REQUIRE(is_stacked_above(conn, desktop, suppressed));
    auto current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    send_client_message(conn, conn.root(), current_desktop, 1);
    observe_title_after_events(conn, owner);
    REQUIRE(is_hidden_offscreen(conn, owner));
    REQUIRE(
        get_window_property_windows(conn.get(), conn.root(), intern_atom(conn.get(), "_NET_CLIENT_LIST_STACKING"))
        == std::vector<xcb_window_t>{ suppressed, owner, desktop }
    );
    REQUIRE(is_stacked_above(conn, desktop, owner));
    REQUIRE(is_stacked_above(conn, owner, suppressed));
    send_client_message(conn, conn.root(), current_desktop, 0);
    observe_title_after_events(conn, owner);
    REQUIRE_FALSE(is_hidden_offscreen(conn, owner));
    REQUIRE(is_stacked_above(conn, owner, desktop));
    for (auto w : { owner, suppressed, desktop }) destroy_window(conn, w);
}
