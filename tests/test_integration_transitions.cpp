#include "ipc_subscription.hpp"
#include "wm_observations.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
#include <xcb/xcb_icccm.h>

using namespace lwm::test;
namespace {
constexpr auto timeout = std::chrono::seconds(2);

void title(X11Connection& conn, xcb_window_t window, std::string const& value)
{
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        window,
        intern_atom(conn.get(), "_NET_WM_NAME"),
        intern_atom(conn.get(), "UTF8_STRING"),
        8,
        value.size(),
        value.data()
    );
    xcb_flush(conn.get());
}
}

TEST_CASE(
    "Integration: kind transitions preserve normal geometry across presentation states",
    "[integration][transition][geometry][rules]"
)
{
    bool rules = GENERATE(false, true);
    auto presentation = GENERATE("normal", "fullscreen", "hidden", "maximized");
    CAPTURE(rules, presentation);
    auto env = TestEnvironment::create(R"(
[workspaces]
count = 2
[[rules]]
match = { title = 'float' }
apply = { floating = true, fullscreen = false }
[[rules]]
match = { title = 'tile' }
apply = { floating = false }
)");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto instance = wm_instance(conn);
    auto peer = create_window(conn, 20, 30, 400, 300);
    map_window(conn, peer);
    REQUIRE(wait_for_active_window(conn, peer, timeout));
    auto window = create_window(conn, 60, 70, 320, 240);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    auto normal = get_window_geometry(conn, window);
    auto peer_tiled = get_window_geometry(conn, peer);
    REQUIRE(normal);
    REQUIRE(peer_tiled);
    auto kind = [&](bool floating)
    {
        if (rules)
            title(conn, window, floating ? "float" : "tile");
        else
            set_window_type(
                conn,
                window,
                intern_atom(conn.get(), floating ? "_NET_WM_WINDOW_TYPE_DIALOG" : "_NET_WM_WINDOW_TYPE_NORMAL")
            );
        REQUIRE(wait_for_condition(
            [&]
            {
                return get_window_property_string(conn.get(), window, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"))
                    == (floating ? "floating" : "tiled");
            },
            timeout
        ));
    };
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    auto horizontal = intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_HORZ");
    auto vertical = intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_VERT");
    if (std::string_view(presentation) == "fullscreen")
    {
        send_client_message(conn, window, state, 1, fullscreen);
        REQUIRE(wait_for_condition([&] { return has_state(conn, window, fullscreen); }, timeout));
    }
    else if (std::string_view(presentation) == "hidden")
    {
        REQUIRE(send_ipc_command(*socket, "workspace switch 1")->starts_with("ok"));
        REQUIRE(wait_for_condition(
            [&]
            {
                auto rectangle = get_window_geometry(conn, window);
                return rectangle && rectangle->x < -10000;
            },
            timeout
        ));
    }
    else if (std::string_view(presentation) == "maximized")
    {
        kind(true);
        send_client_message(conn, window, state, 1, horizontal, vertical);
        REQUIRE(wait_for_condition([&] { return has_state(conn, window, horizontal); }, timeout));
        kind(false);
    }
    kind(true);
    // Metadata changes keep fullscreen intent; the rule explicitly clears it.
    send_client_message(conn, window, state, 0, fullscreen);
    send_client_message(conn, window, state, 0, horizontal, vertical);
    REQUIRE(send_ipc_command(*socket, "workspace switch 0")->starts_with("ok"));
    REQUIRE(wait_for_condition([&] { return get_window_geometry(conn, window) == normal; }, timeout));
    REQUIRE(get_window_geometry(conn, peer) != peer_tiled);
    WindowGeometry moved{ 55, 66, 311, 217 };
    uint32_t values[] = { 55, 66, 311, 217 };
    xcb_configure_window(
        conn.get(),
        window,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        values
    );
    xcb_flush(conn.get());
    REQUIRE(wait_for_condition([&] { return get_window_geometry(conn, window) == moved; }, timeout));
    kind(false);
    REQUIRE(wait_for_condition([&] { return get_window_geometry(conn, peer) == peer_tiled; }, timeout));
    kind(true);
    REQUIRE(wait_for_condition([&] { return get_window_geometry(conn, window) == moved; }, timeout));
    REQUIRE(wm_instance(conn) == instance);
    destroy_window(conn, window);
    destroy_window(conn, peer);
}

TEST_CASE(
    "Integration: transient parent changes reconcile fullscreen visibility",
    "[integration][transition][property][fullscreen]"
)
{
    bool initially_transient = false;
    SECTION("Attach a suppressed dialog to the fullscreen owner") { initially_transient = false; }
    SECTION("Detach a visible dialog from the fullscreen owner") { initially_transient = true; }
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto owner = create_window(conn, 20, 30, 400, 300);
    map_window(conn, owner);
    REQUIRE(wait_for_active_window(conn, owner, timeout));
    auto dialog = create_window(conn, 60, 70, 320, 240);
    set_window_type(conn, dialog, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    if (initially_transient)
        xcb_icccm_set_wm_transient_for(conn.get(), dialog, owner);
    map_window(conn, dialog);
    REQUIRE(wait_for_active_window(conn, dialog, timeout));
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    send_client_message(conn, owner, intern_atom(conn.get(), "_NET_WM_STATE"), 1, fullscreen);
    REQUIRE(wait_for_condition([&] { return has_state(conn, owner, fullscreen); }, timeout));
    auto has_visibility = [&](bool visible)
    {
        auto rect = get_window_geometry(conn, dialog);
        return rect && rect->width == 320 && rect->height == 240
            && (visible ? rect->x >= 0 && rect->y >= 0 : rect->x < -10000);
    };
    REQUIRE(wait_for_condition([&] { return has_visibility(initially_transient); }, timeout));

    if (initially_transient)
        xcb_delete_property(conn.get(), dialog, intern_atom(conn.get(), "WM_TRANSIENT_FOR"));
    else
        xcb_icccm_set_wm_transient_for(conn.get(), dialog, owner);
    observe_title_after_events(conn, dialog);
    REQUIRE(wait_for_condition([&] { return has_visibility(!initially_transient); }, timeout));
    REQUIRE(has_state(conn, owner, fullscreen));
    destroy_window(conn, dialog);
    destroy_window(conn, owner);
}

TEST_CASE(
    "Integration: reclassifying an unarranged client preserves its initial geometry",
    "[integration][transition][geometry][property]"
)
{
    auto env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto window = create_window(conn, 60, 70, 320, 240);
    auto desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    uint32_t workspace = 1;
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, desktop, XCB_ATOM_CARDINAL, 32, 1, &workspace);
    map_window(conn, window);
    REQUIRE(wait_for_condition(
        [&]
        {
            return get_window_property_string(conn.get(), window, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"))
                == "tiled";
        },
        timeout
    ));
    observe_title_after_events(conn, window);
    REQUIRE(require_property_cardinal(conn.get(), window, desktop) == 1);
    REQUIRE(get_window_geometry(conn, window));
    REQUIRE(get_window_geometry(conn, window)->x < -10000);

    SECTION("Window type changes to dialog")
    {
        set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    }
    SECTION("Window becomes transient") { xcb_icccm_set_wm_transient_for(conn.get(), window, conn.root()); }
    observe_title_after_events(conn, window);
    auto reply = send_ipc_command(*socket, "workspace switch 1");
    REQUIRE(reply);
    REQUIRE(reply->starts_with("ok"));
    REQUIRE(wait_for_condition(
        [&] { return get_window_geometry(conn, window) == WindowGeometry{ 60, 70, 320, 240 }; },
        timeout
    ));
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: compound rules compose placement kind visibility and focus",
    "[integration][transition][rules][sequence]"
)
{
    auto env = TestEnvironment::create(R"(
[workspaces]
count = 2
[[rules]]
match = { title = 'hidden' }
apply = { floating = false, workspace = 1, sticky = false, fullscreen = true }
[[rules]]
match = { title = 'shown' }
apply = { floating = true, workspace = 0, sticky = true, fullscreen = false, geometry = { x = 60, y = 70, width = 310, height = 210 } }
)");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto background = create_window(conn, 10, 10, 200, 200);
    map_window(conn, background);
    REQUIRE(wait_for_active_window(conn, background, timeout));
    auto window = create_window(conn, 60, 70, 310, 210);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    title(conn, window, "shown");
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    for (int iteration = 0; iteration < 12; ++iteration)
    {
        CAPTURE(iteration);
        for (bool hidden : { true, false })
        {
            title(conn, window, hidden ? "hidden" : "shown");
            REQUIRE(wait_for_condition(
                [&]
                {
                    auto reply = send_ipc_command(*socket, "window list");
                    if (!reply || !reply->starts_with("ok "))
                        return false;
                    auto data = nlohmann::json::parse(reply->substr(3));
                    for (auto const& client : data.at("windows"))
                        if (client.at("id") == window)
                            return client.at("title") == (hidden ? "hidden" : "shown")
                                && client.at("workspace") == (hidden ? 1 : 0)
                                && client.at("kind") == (hidden ? "tiled" : "floating")
                                && client.at("fullscreen") == hidden && client.at("sticky") == !hidden;
                    return false;
                },
                timeout
            ));
            REQUIRE(wait_for_condition(
                [&]
                {
                    auto rect = get_window_geometry(conn, window);
                    return rect && (hidden ? rect->x < -10000 : *rect == WindowGeometry{ 60, 70, 310, 210 });
                },
                timeout
            ));
            REQUIRE(wait_for_active_window(conn, background, timeout));
        }
        auto reply = send_ipc_command(*socket, "focus window=" + std::to_string(window));
        REQUIRE(reply);
        REQUIRE(reply->starts_with("ok"));
        REQUIRE(wait_for_active_window(conn, window, timeout));
    }
    destroy_window(conn, window);
    destroy_window(conn, background);
}

TEST_CASE(
    "Integration: fullscreen and maximize preserve one normal rectangle",
    "[integration][transition][geometry][wm_state]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 60, 70, 310, 210);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    auto hints = [&](int x, int y, int width, int height)
    {
        xcb_size_hints_t value{ };
        value.flags = XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_US_SIZE;
        value.x = x;
        value.y = y;
        value.width = width;
        value.height = height;
        xcb_icccm_set_wm_normal_hints(conn.get(), window, &value);
        xcb_flush(conn.get());
    };
    hints(60, 70, 310, 210);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto horizontal = intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_HORZ");
    auto vertical = intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_VERT");
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    send_client_message(conn, window, state, 1, horizontal, vertical);
    REQUIRE(wait_for_condition(
        [&] { return has_state(conn, window, horizontal) && has_state(conn, window, vertical); },
        timeout
    ));
    send_client_message(conn, window, state, 1, fullscreen, horizontal);
    REQUIRE(wait_for_condition(
        [&]
        {
            return has_state(conn, window, fullscreen) && !has_state(conn, window, horizontal)
                && !has_state(conn, window, vertical);
        },
        timeout
    ));
    hints(140, 150, 420, 280);
    // Observe completion of the hints before the following request on the same X connection.
    send_client_message(conn, window, state, 0, fullscreen);
    REQUIRE(wait_for_condition(
        [&] { return get_window_geometry(conn, window) == WindowGeometry{ 140, 150, 420, 280 }; },
        timeout
    ));
    send_client_message(conn, window, state, 1, horizontal);
    REQUIRE(wait_for_condition(
        [&]
        {
            auto rect = get_window_geometry(conn, window);
            return rect && rect->width == conn.screen()->width_in_pixels && rect->y == 150 && rect->height == 280;
        },
        timeout
    ));
    send_client_message(conn, window, state, 0, horizontal);
    REQUIRE(wait_for_condition(
        [&] { return get_window_geometry(conn, window) == WindowGeometry{ 140, 150, 420, 280 }; },
        timeout
    ));
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: managed configure requests receive exactly one synthetic acknowledgement",
    "[integration][transition][configure]"
)
{
    bool changed = GENERATE(false, true);
    std::string_view mode = GENERATE("floating", "fullscreen", "hidden", "hidden_tiled");
    CAPTURE(changed, mode);
    auto env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 60, 70, 310, 210);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn.get(), window, XCB_CW_EVENT_MASK, &mask);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    if (mode == "fullscreen")
    {
        auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
        send_client_message(conn, window, intern_atom(conn.get(), "_NET_WM_STATE"), 1, fullscreen);
        REQUIRE(wait_for_condition([&] { return has_state(conn, window, fullscreen); }, timeout));
    }
    auto rect = get_window_geometry(conn, window);
    REQUIRE(rect);
    bool hidden = mode == "hidden" || mode == "hidden_tiled";
    if (hidden)
    {
        auto reply = send_ipc_command(*socket, "workspace switch 1");
        REQUIRE(reply);
        REQUIRE(reply->starts_with("ok"));
        if (mode == "hidden_tiled")
            set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_NORMAL"));
    }
    observe_title_after_events(conn, window);
    REQUIRE(get_window_geometry(conn, window));
    while (auto* event = xcb_poll_for_event(conn.get())) free(event);
    uint32_t values[] = { static_cast<uint32_t>(rect->width + (changed ? 100 : 0)), rect->height };
    xcb_configure_window(conn.get(), window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    observe_title_after_events(conn, window);
    REQUIRE(get_window_geometry(conn, window)); // Roundtrip also collects queued X events.
    auto expected_width = (mode == "fullscreen" || mode == "hidden_tiled") ? rect->width : values[0];
    size_t acknowledgements = 0;
    while (auto* event = xcb_poll_for_event(conn.get()))
    {
        if (event->response_type == (XCB_CONFIGURE_NOTIFY | 0x80))
        {
            auto const configure = *reinterpret_cast<xcb_configure_notify_event_t*>(event);
            free(event);
            REQUIRE(configure.window == window);
            CHECK(configure.width == expected_width);
            CHECK(configure.x == rect->x);
            CHECK(configure.y == rect->y);
            CHECK(configure.height == values[1]);
            ++acknowledgements;
        }
        else
            free(event);
    }
    REQUIRE(acknowledgements == 1);
    if (mode != "hidden_tiled")
    {
        if (hidden)
        {
            auto reply = send_ipc_command(*socket, "workspace switch 0");
            REQUIRE(reply);
            REQUIRE(reply->starts_with("ok"));
        }
        REQUIRE(wait_for_condition(
            [&]
            {
                auto actual = get_window_geometry(conn, window);
                return actual && actual->width == expected_width && actual->height == rect->height;
            },
            timeout
        ));
    }
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: subscriptions publish settled focus and lifecycle order",
    "[integration][transition][subscribe]"
)
{
    auto env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    Subscriber subscriber(
        *path,
        "workspace_switch,focus_change,window_map,window_unmap,layout_change,config_reload,key_action"
    );
    auto event = [&]
    {
        auto line = subscriber.line();
        REQUIRE_FALSE(line.empty());
        return nlohmann::json::parse(line);
    };
    auto first = create_window(conn, 10, 10, 200, 200);
    map_window(conn, first);
    CHECK(event().at("event") == "focus_change");
    CHECK(event().at("event") == "window_map");
    auto switched = send_ipc_command(*path, "workspace switch 1");
    REQUIRE(switched);
    REQUIRE(switched->starts_with("ok"));
    CHECK(event().at("event") == "workspace_switch");
    auto second = create_window(conn, 10, 10, 200, 200);
    map_window(conn, second);
    CHECK(event().at("event") == "focus_change");
    CHECK(event().at("event") == "window_map");
    auto focused = send_ipc_command(*path, "focus window=" + std::to_string(first));
    REQUIRE(focused);
    REQUIRE(focused->starts_with("ok"));
    auto workspace = event();
    CHECK(workspace.at("event") == "workspace_switch");
    CHECK(workspace.at("from") == 1);
    CHECK(workspace.at("to") == 0);
    auto focus = event();
    CHECK(focus.at("event") == "focus_change");
    CHECK(focus.at("window") == first);
    REQUIRE(wait_for_active_window(conn, first, timeout));
    auto first_geometry = get_window_geometry(conn, first);
    auto second_geometry = get_window_geometry(conn, second);
    REQUIRE(first_geometry);
    REQUIRE(second_geometry);
    CHECK(first_geometry->x >= 0);
    CHECK(second_geometry->x < -10000);
    auto reply = send_ipc_command(*path, "window list");
    REQUIRE(reply);
    CHECK(nlohmann::json::parse(reply->substr(3)).at("focused") == first);
    CHECK_FALSE(subscriber.reader.read(subscriber.fd, std::chrono::milliseconds(30)));
    destroy_window(conn, second);
    destroy_window(conn, first);
}

TEST_CASE("Integration: tiled resize motion stops at queued button release", "[integration][transition][drag]")
{
    auto env = TestEnvironment::create("[appearance]\npadding = 10\nborder_width = 1\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto first = create_window(conn, 10, 10, 200, 200);
    auto second = create_window(conn, 10, 10, 200, 200);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, timeout));
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, timeout));
    auto left = get_window_geometry(conn, first);
    auto right = get_window_geometry(conn, second);
    REQUIRE(left);
    REQUIRE(right);
    xcb_window_t left_window = first;
    if (left->x > right->x)
    {
        std::swap(left, right);
        left_window = second;
    }
    int16_t start_x = (left->x + left->width + right->x) / 2;
    int16_t y = left->y + left->height / 2;
    auto pointer = [&](uint8_t type, int16_t x)
    {
        xcb_button_press_event_t event{ };
        event.response_type = type;
        event.detail = type == XCB_MOTION_NOTIFY ? 0 : 1;
        event.root = conn.root();
        event.event = conn.root();
        event.root_x = event.event_x = x;
        event.root_y = event.event_y = y;
        event.same_screen = 1;
        uint32_t mask = type == XCB_MOTION_NOTIFY ? XCB_EVENT_MASK_POINTER_MOTION
            : type == XCB_BUTTON_PRESS            ? XCB_EVENT_MASK_BUTTON_PRESS
                                                  : XCB_EVENT_MASK_BUTTON_RELEASE;
        xcb_send_event(conn.get(), 0, conn.root(), mask, reinterpret_cast<char*>(&event));
    };
    pointer(XCB_BUTTON_PRESS, start_x);
    pointer(XCB_MOTION_NOTIFY, start_x + 40);
    xcb_flush(conn.get());
    REQUIRE(wait_for_condition(
        [&]
        {
            auto current = get_window_geometry(conn, left_window);
            return current && current->width > left->width + 30;
        },
        timeout
    ));
    for (int offset = 41; offset <= 80; ++offset) pointer(XCB_MOTION_NOTIFY, start_x + offset);
    pointer(XCB_BUTTON_RELEASE, start_x + 80);
    pointer(XCB_MOTION_NOTIFY, start_x + 250);
    // The unflushed pointer sequence and this marker reach the WM together, in order.
    observe_title_after_events(conn, first);
    auto current = get_window_geometry(conn, left_window);
    REQUIRE(current);
    CHECK(std::abs(static_cast<int>(current->width) - left->width - 80) <= 2);
    destroy_window(conn, first);
    destroy_window(conn, second);
}

TEST_CASE(
    "Integration: withdrawal clears focused state and remapping uses normal registration",
    "[integration][transition][lifecycle]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto first = create_window(conn, 10, 10, 200, 200);
    auto second = create_window(conn, 10, 10, 200, 200);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, timeout));
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, timeout));
    auto focused = intern_atom(conn.get(), "_NET_WM_STATE_FOCUSED");
    REQUIRE(wait_for_condition([&] { return has_state(conn, second, focused); }, timeout));
    xcb_unmap_window(conn.get(), second);
    xcb_flush(conn.get());
    REQUIRE(wait_for_active_window(conn, first, timeout));
    REQUIRE(wait_for_condition([&] { return !has_state(conn, second, focused); }, timeout));
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, timeout));
    REQUIRE(wait_for_condition([&] { return has_state(conn, second, focused); }, timeout));
    destroy_window(conn, second);
    REQUIRE(wait_for_active_window(conn, first, timeout));
    destroy_window(conn, first);
}

TEST_CASE("Integration: state requests leave dock and desktop windows alone", "[integration][transition][geometry]")
{
    char const* type = nullptr;
    SECTION("dock") { type = "_NET_WM_WINDOW_TYPE_DOCK"; }
    SECTION("desktop") { type = "_NET_WM_WINDOW_TYPE_DESKTOP"; }
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 15, 25, 500, 40);
    set_window_type(conn, window, intern_atom(conn.get(), type));
    map_window(conn, window);
    auto classification = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    REQUIRE(wait_for_condition(
        [&] { return get_window_property_string(conn.get(), window, classification).has_value(); },
        timeout
    ));
    auto before = get_window_geometry(conn, window);
    REQUIRE(before);
    auto horizontal = intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_HORZ");
    auto marker = create_window(conn, 10, 10, 100, 100);
    map_window(conn, marker);
    REQUIRE(wait_for_active_window(conn, marker, timeout));
    send_client_message(conn, window, intern_atom(conn.get(), "_NET_WM_STATE"), 1, horizontal);
    // A marker on the same connection proves the request was handled.
    observe_title_after_events(conn, marker);
    // Fixtures have no maximized presentation, so none is advertised.
    CHECK_FALSE(has_state(conn, window, horizontal));
    CHECK(get_window_geometry(conn, window) == before);
    destroy_window(conn, marker);
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: restart adoption preserves deiconified non-active clients",
    "[integration][transition][adoption]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    auto first = create_window(conn, 10, 10, 200, 200);
    xcb_icccm_wm_hints_t hints{ };
    hints.flags = XCB_ICCCM_WM_HINT_STATE;
    hints.initial_state = XCB_ICCCM_WM_STATE_ICONIC;
    xcb_icccm_set_wm_hints(conn.get(), first, &hints);
    map_window(conn, first);
    auto clients = [&]
    {
        auto reply = send_ipc_command(*path, "window list");
        REQUIRE(reply);
        REQUIRE(reply->starts_with("ok "));
        return nlohmann::json::parse(reply->substr(3));
    };
    REQUIRE(wait_for_condition([&] { return clients().at("windows").size() == 1; }, timeout));
    CHECK(clients().at("windows").at(0).at("iconic") == true);
    auto focused = send_ipc_command(*path, "focus window=" + std::to_string(first));
    REQUIRE(focused);
    REQUIRE(focused->starts_with("ok"));
    REQUIRE(wait_for_active_window(conn, first, timeout));
    auto second = create_window(conn, 10, 10, 200, 200);
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, timeout));
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    auto restarted = send_ipc_command(*path, "restart");
    REQUIRE(restarted);
    REQUIRE(restarted->starts_with("ok"));
    REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    REQUIRE(wait_for_active_window(conn, second, timeout));
    auto snapshot = clients();
    REQUIRE(snapshot.at("windows").size() == 2);
    for (auto const& client : snapshot.at("windows")) CHECK(client.at("iconic") == false);
    auto rect = get_window_geometry(conn, first);
    REQUIRE(rect);
    CHECK(rect->x >= 0);
    destroy_window(conn, first);
    destroy_window(conn, second);
}

TEST_CASE(
    "Integration: dock batches preserve adoption placement and refresh workareas",
    "[integration][transition][dock][adoption]"
)
{
    auto& server = X11TestEnvironment::instance();
    if (!server.available())
    {
        REQUIRE(std::getenv("LWM_TEST_REQUIRE_X11") == nullptr);
        SKIP("X11 unavailable");
    }
    X11Connection conn;
    REQUIRE(conn.ok());
    auto strut = intern_atom(conn.get(), "_NET_WM_STRUT");
    auto partial = intern_atom(conn.get(), "_NET_WM_STRUT_PARTIAL");
    auto set_strut = [&](xcb_window_t window, xcb_atom_t property, uint32_t top)
    {
        uint32_t values[12] = { 0, 0, top, 0 };
        xcb_change_property(
            conn.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            property,
            XCB_ATOM_CARDINAL,
            32,
            property == partial ? 12 : 4,
            values
        );
        xcb_flush(conn.get());
    };
    auto dock = [&](uint32_t top)
    {
        auto window = create_window(conn, 0, 0, 200, 20);
        set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DOCK"));
        set_strut(window, strut, top);
        map_window(conn, window);
        return window;
    };
    auto dialog = [&]
    {
        auto window = create_window(conn, 0, 0, 200, 100);
        set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
        map_window(conn, window);
        return window;
    };
    auto first_dock = dock(20);
    auto second_dock = dock(40);
    auto first = dialog();
    auto third_dock = dock(80);
    auto second = dialog();
    // A reply on this connection ensures all windows exist before the startup scan.
    REQUIRE(get_window_geometry(conn, second));
    LwmProcess wm(server.display(), R"(
[appearance]
padding = 0
border_width = 0
[[rules]]
match = { type = "dialog" }
apply = { center = true }
)");
    REQUIRE(wait_for_wm_ready(conn, timeout));
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    REQUIRE(send_ipc_command(*path, "ping"));
    auto screen_height = conn.screen()->height_in_pixels;
    auto first_rect = get_window_geometry(conn, first);
    auto second_rect = get_window_geometry(conn, second);
    REQUIRE(first_rect);
    REQUIRE(second_rect);
    CHECK(first_rect->y == 40 + (screen_height - 40 - 100) / 2);
    CHECK(second_rect->y == 80 + (screen_height - 80 - 100) / 2);
    auto workarea_top = [&]() -> uint32_t
    {
        auto* reply = xcb_get_property_reply(
            conn.get(),
            xcb_get_property(
                conn.get(),
                0,
                conn.root(),
                intern_atom(conn.get(), "_NET_WORKAREA"),
                XCB_ATOM_CARDINAL,
                0,
                4
            ),
            nullptr
        );
        uint32_t top = UINT32_MAX;
        if (reply && xcb_get_property_value_length(reply) == 16)
            top = static_cast<uint32_t*>(xcb_get_property_value(reply))[1];
        free(reply);
        return top;
    };
    REQUIRE(wait_for_condition([&] { return workarea_top() == 80; }, timeout));
    auto tiled = create_window(conn, 10, 10, 200, 100);
    map_window(conn, tiled);
    REQUIRE(wait_for_active_window(conn, tiled, timeout));
    auto check_area = [&](uint32_t top)
    {
        REQUIRE(wait_for_condition(
            [&]
            {
                auto rect = get_window_geometry(conn, tiled);
                return workarea_top() == top && rect && rect->y == top && rect->height == screen_height - top;
            },
            timeout
        ));
    };
    check_area(80);
    set_strut(second_dock, partial, 120);
    check_area(120);
    xcb_delete_property(conn.get(), second_dock, partial);
    xcb_flush(conn.get());
    check_area(80);
    destroy_window(conn, third_dock);
    check_area(40);
    destroy_window(conn, second_dock);
    check_area(20);
    destroy_window(conn, first_dock);
    check_area(0);
    destroy_window(conn, tiled);
    destroy_window(conn, first);
    destroy_window(conn, second);
}

TEST_CASE(
    "Integration: subscriptions and state snapshots have a recoverable ordering boundary",
    "[integration][ipc][subscribe]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    Subscriber subscriber(*path, "state_change,window_map");
    auto state = [&]
    {
        auto reply = send_ipc_command(*path, "state");
        REQUIRE(reply);
        REQUIRE(reply->starts_with("ok "));
        return nlohmann::json::parse(reply->substr(3));
    };
    auto window = create_window(conn, 30, 40, 200, 150);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    auto snapshot = state();
    CHECK(snapshot.at("windows").at("focused") == window);
    CHECK(snapshot.at("windows").at("windows").size() == 1);
    CHECK(snapshot.at("workspaces").contains("monitors"));
    CHECK(snapshot.at("scratchpads").contains("named"));
    uint64_t previous = 0;
    for (int i = 0; i < 2; ++i)
    {
        auto line = subscriber.line();
        REQUIRE_FALSE(line.empty());
        auto event = nlohmann::json::parse(line);
        CHECK(event.at("instance") == snapshot.at("instance"));
        auto sequence = event.at("sequence").get<uint64_t>();
        CHECK(sequence > previous);
        CHECK(sequence <= snapshot.at("sequence").get<uint64_t>());
        previous = sequence;
    }
    // A metadata-only update must invalidate snapshots even when no rule matches.
    title(conn, window, "snapshot-new-title");
    auto line = subscriber.line();
    REQUIRE_FALSE(line.empty());
    auto changed = nlohmann::json::parse(line);
    CHECK(changed.at("event") == "state_change");
    CHECK(changed.at("sequence").get<uint64_t>() > snapshot.at("sequence").get<uint64_t>());
    auto current = state();
    CHECK(current.at("windows").at("windows").at(0).at("title") == "snapshot-new-title");
    CHECK(current.at("sequence") == changed.at("sequence"));
    auto logging = send_ipc_command(*path, "log status");
    REQUIRE(logging);
    REQUIRE(logging->starts_with("ok "));
    CHECK(nlohmann::json::parse(logging->substr(3)).at("active") == true);
    CHECK(state().at("sequence") == current.at("sequence"));
    // Include already buffered records when checking for a feedback loop.
    CHECK_FALSE(subscriber.reader.read(subscriber.fd, std::chrono::milliseconds(30)));
    auto previous_wm = wm_instance(conn);
    REQUIRE(previous_wm);
    REQUIRE(send_ipc_command(*path, "restart"));
    REQUIRE(wait_for_wm_restart(conn, timeout, *previous_wm));
    REQUIRE(wait_for_condition([&] { return send_ipc_command(*path, "ping") == "ok pong"; }, timeout));
    Subscriber reconnected(*path, "state_change");
    CHECK(state().at("instance") != snapshot.at("instance"));
    destroy_window(conn, window);
}

TEST_CASE("Integration: application state requests do not replay rule placement", "[integration][transition][rules]")
{
    auto env = TestEnvironment::create(R"(
[[rules]]
match = { class = "Positioned" }
apply = { floating = true, below = true, geometry = { x = 60, y = 70, width = 300, height = 200 } }
)");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 60, 70, 300, 200);
    set_window_wm_class(conn, window, "test", "Positioned");
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");
    auto below = intern_atom(conn.get(), "_NET_WM_STATE_BELOW");
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    send_client_message(
        conn,
        window,
        intern_atom(conn.get(), "_NET_MOVERESIZE_WINDOW"),
        (1u << 8) | (1u << 9),
        360,
        270,
        0,
        0
    );
    REQUIRE(wait_for_condition(
        [&] { return get_window_geometry(conn, window) == WindowGeometry{ 360, 270, 300, 200 }; },
        timeout
    ));
    send_client_message(conn, window, state, 1, above, above, 0, 0);
    observe_title_after_events(conn, window);
    CHECK(get_window_geometry(conn, window) == WindowGeometry{ 360, 270, 300, 200 });
    CHECK(property_has_atom(conn.get(), window, state, above));
    CHECK_FALSE(property_has_atom(conn.get(), window, state, below));
    // A duplicate toggle is one operation; invalid actions are ignored.
    send_client_message(conn, window, state, 2, above, above, 0, 0);
    observe_title_after_events(conn, window);
    CHECK_FALSE(property_has_atom(conn.get(), window, state, above));
    send_client_message(conn, window, state, 99, below, 0, 0, 0);
    observe_title_after_events(conn, window);
    CHECK_FALSE(property_has_atom(conn.get(), window, state, below));
    send_client_message(conn, window, state, 1, below, 0, 0, 0);
    send_client_message(conn, window, state, 1, fullscreen, 0, 0, 0);
    observe_title_after_events(conn, window);
    CHECK(property_has_atom(conn.get(), window, state, fullscreen));
    send_client_message(conn, window, state, 0, fullscreen, 0, 0, 0);
    observe_title_after_events(conn, window);
    CHECK(property_has_atom(conn.get(), window, state, below));
    CHECK(get_window_geometry(conn, window) == WindowGeometry{ 360, 270, 300, 200 });
    // Reload explicitly reapplies even unchanged placement actions.
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    REQUIRE(send_ipc_command(*socket, "reload-config")->starts_with("ok "));
    CHECK(get_window_geometry(conn, window) == WindowGeometry{ 60, 70, 300, 200 });
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: explicit preferences survive defaults modal fullscreen and restart",
    "[integration][transition][rules][restart]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 60, 70, 300, 200);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_UTILITY"));
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto taskbar = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_TASKBAR");
    auto below = intern_atom(conn.get(), "_NET_WM_STATE_BELOW");
    auto modal = intern_atom(conn.get(), "_NET_WM_STATE_MODAL");
    auto above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");
    send_client_message(conn, window, state, 0, taskbar, 0, 0, 0);
    send_client_message(conn, window, state, 1, below, modal, 0, 0);
    observe_title_after_events(conn, window);
    CHECK_FALSE(property_has_atom(conn.get(), window, state, taskbar));
    CHECK(property_has_atom(conn.get(), window, state, above));
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto instance = wm_instance(conn);
    REQUIRE(instance);
    REQUIRE(send_ipc_command(*socket, "restart")->starts_with("ok "));
    REQUIRE(wait_for_wm_restart(conn, timeout, *instance));
    send_client_message(conn, window, state, 0, modal, 0, 0, 0);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_NORMAL"));
    observe_title_after_events(conn, window);
    CHECK_FALSE(property_has_atom(conn.get(), window, state, taskbar));
    CHECK(property_has_atom(conn.get(), window, state, below));
    CHECK_FALSE(property_has_atom(conn.get(), window, state, above));
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: generated state requests converge to the declared preferences",
    "[integration][transition][sequence]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 60, 70, 300, 200);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto taskbar = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_TASKBAR");
    auto pager = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_PAGER");
    auto modal = intern_atom(conn.get(), "_NET_WM_STATE_MODAL");
    auto above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");
    auto below = intern_atom(conn.get(), "_NET_WM_STATE_BELOW");
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    uint32_t seed = 0x14537;
    if (auto* value = std::getenv("LWM_TEST_SEQUENCE_SEED"))
        seed = static_cast<uint32_t>(std::stoul(value));
    size_t steps = 160;
    if (auto* value = std::getenv("LWM_TEST_SEQUENCE_STEPS"))
        steps = std::stoul(value);
    CAPTURE(seed);
    auto random = [&]
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    bool skip_taskbar = false, skip_pager = false, is_modal = false, is_fullscreen = false;
    int layer = 0;
    // Establish explicit false choices before varying classification defaults.
    send_client_message(conn, window, state, 0, taskbar, pager, 0, 0);
    send_client_message(conn, window, state, 0, above, below, 0, 0);
    std::string trace;
    for (size_t step = 0; step < steps; ++step)
    {
        auto before_instance = wm_instance(conn);
        REQUIRE(before_instance);
        auto op = random() % 9;
        bool enable = (random() & 1) != 0;
        trace += std::to_string(op) + ":" + std::to_string(enable) + " ";
        INFO("Replay operations: " << trace);
        CAPTURE(step);
        if (op < 6)
        {
            auto atom = std::array{ taskbar, pager, modal, above, below, fullscreen }[op];
            send_client_message(conn, window, state, enable ? 1 : 0, atom, atom, 0, 0);
            if (op == 0)
                skip_taskbar = enable;
            if (op == 1)
                skip_pager = enable;
            if (op == 2)
                is_modal = enable;
            if (op == 3)
            {
                if (enable)
                    layer = 1;
                else if (layer == 1)
                    layer = 0;
            }
            if (op == 4)
            {
                if (enable)
                    layer = -1;
                else if (layer == -1)
                    layer = 0;
            }
            if (op == 5)
                is_fullscreen = enable;
        }
        else if (op == 6)
        {
            set_window_type(
                conn,
                window,
                intern_atom(conn.get(), enable ? "_NET_WM_WINDOW_TYPE_UTILITY" : "_NET_WM_WINDOW_TYPE_NORMAL")
            );
        }
        else if (op == 7)
        {
            auto socket = wait_for_ipc_socket_path(conn);
            REQUIRE(socket);
            auto instance = wm_instance(conn);
            REQUIRE(instance);
            auto reply = send_ipc_command(*socket, "restart");
            REQUIRE(reply);
            REQUIRE(reply->starts_with("ok "));
            REQUIRE(wait_for_wm_restart(conn, timeout, *instance));
        }
        else
        {
            send_client_message(conn, window, state, 42, fullscreen, modal, 0, 0);
        }
        observe_title_after_events(conn, window);
        if (op != 7)
            CHECK(wm_instance(conn) == before_instance);
        CHECK(property_has_atom(conn.get(), window, state, taskbar) == skip_taskbar);
        CHECK(property_has_atom(conn.get(), window, state, pager) == skip_pager);
        CHECK(property_has_atom(conn.get(), window, state, modal) == is_modal);
        CHECK(property_has_atom(conn.get(), window, state, fullscreen) == is_fullscreen);
        CHECK(property_has_atom(conn.get(), window, state, above) == (!is_fullscreen && (is_modal || layer == 1)));
        CHECK(property_has_atom(conn.get(), window, state, below) == (!is_fullscreen && !is_modal && layer == -1));
    }
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: repeated fullscreen request selects its existing fullscreen client",
    "[integration][transition][fullscreen]"
)
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    auto first = create_window(conn, 10, 10, 200, 150), second = create_window(conn, 10, 10, 200, 150);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, timeout));
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, timeout));
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    for (auto owner : { first, second, first })
    {
        send_client_message(conn, owner, state, 1, fullscreen, 0, 0, 0);
        observe_title_after_events(conn, owner);
        auto active = get_window_geometry(conn, owner),
             hidden = get_window_geometry(conn, owner == first ? second : first);
        REQUIRE(active);
        REQUIRE(hidden);
        CHECK(active->x == 0);
        CHECK(hidden->x < -10000);
    }
    destroy_window(conn, first);
    destroy_window(conn, second);
}

TEST_CASE(
    "Integration: floating an unarranged tile with off-monitor geometry places it on its monitor",
    "[integration][transition][geometry]"
)
{
    auto env = TestEnvironment::create(R"(
[workspaces]
count = 2
[[rules]]
match = { class = "Parked" }
apply = { workspace = 1 }
)");
    REQUIRE(env);
    auto& conn = env->conn;
    // A client may request a position saved from a monitor that no longer exists.
    auto window = create_window(conn, -5000, 10, 300, 200);
    set_window_wm_class(conn, window, "parked", "Parked");
    map_window(conn, window);
    auto window_class = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    REQUIRE(wait_for_condition(
        [&] { return get_window_property_string(conn.get(), window, window_class) == "tiled"; },
        timeout
    ));
    // Its workspace is not arranged, so the requested rectangle is its only tiled geometry.
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_UTILITY"));
    observe_title_after_events(conn, window);
    REQUIRE(get_window_property_string(conn.get(), window, window_class) == "floating");
    send_client_message(conn, conn.root(), intern_atom(conn.get(), "_NET_CURRENT_DESKTOP"), 1);
    observe_title_after_events(conn, window);
    auto rect = get_window_geometry(conn, window);
    REQUIRE(rect);
    CHECK(rect->x >= 0);
    CHECK(rect->x + rect->width <= conn.screen()->width_in_pixels);
    CHECK(rect->y >= 0);
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: fullscreen rule assignments preserve claims across reload metadata and restart",
    "[integration][transition][fullscreen][rules][reload][restart]"
)
{
    auto replay = GENERATE("reload", "metadata", "restart then reload");
    bool hidden = GENERATE(false, true);
    CAPTURE(replay, hidden);
    auto env = TestEnvironment::create(R"(
[workspaces]
count = 2
[[rules]]
match = { class = "FullscreenChanged" }
apply = { fullscreen = true, skip_pager = true }
[[rules]]
match = { class = "Fullscreen.*" }
apply = { fullscreen = true }
[[rules]]
match = { class = "NotFullscreen" }
apply = { fullscreen = false }
)");
    REQUIRE(env);
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    auto minimized = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    auto first = create_window(conn, 10, 10, 320, 240);
    auto second = create_window(conn, 20, 20, 320, 240);
    for (auto window : { first, second })
    {
        set_window_wm_class(conn, window, "test", "FullscreenOriginal");
        map_window(conn, window);
        REQUIRE(wait_for_active_window(conn, window, timeout));
        REQUIRE(has_state(conn, window, fullscreen));
    }
    auto expect_owner = [&](xcb_window_t owner, xcb_window_t suppressed)
    {
        auto rectangle = get_window_geometry(conn, owner);
        REQUIRE(rectangle);
        CHECK(rectangle->x == 0);
        CHECK(rectangle->width == conn.screen()->width_in_pixels);
        CHECK(is_hidden_offscreen(conn, suppressed));
        CHECK(has_state(conn, owner, fullscreen));
        CHECK(get_window_property_window(conn.get(), conn.root(), intern_atom(conn.get(), "_NET_ACTIVE_WINDOW")) == owner);
    };
    // Registration order disagrees with claim order. Both clients already have
    // fullscreen enabled; this explicit request gives the older one priority.
    send_client_message(conn, first, state, 1, fullscreen, fullscreen);
    observe_title_after_events(conn, first);
    expect_owner(first, second);
    if (hidden)
        ipc_ok(*socket, "workspace switch 1");

    if (std::string_view(replay) == "metadata")
    {
        // Changing an unrelated rule field replays the entire rule bundle.
        set_window_wm_class(conn, second, "test", "FullscreenChanged");
        observe_title_after_events(conn, first);
        CHECK(has_state(conn, second, intern_atom(conn.get(), "_NET_WM_STATE_SKIP_PAGER")));
    }
    else
    {
        if (std::string_view(replay) == "restart then reload")
        {
            auto instance = wm_instance(conn);
            REQUIRE(instance);
            ipc_ok(*socket, "restart");
            REQUIRE(wait_for_wm_restart(conn, timeout, *instance));
        }
        ipc_ok(*socket, "reload-config");
    }
    if (hidden)
        ipc_ok(*socket, "workspace switch 0");
    expect_owner(first, second);
    CHECK(has_state(conn, second, fullscreen));

    // A real setting transition still creates a new claim when re-enabled.
    set_window_wm_class(conn, second, "test", "NotFullscreen");
    observe_title_after_events(conn, first);
    CHECK_FALSE(has_state(conn, second, fullscreen));
    expect_owner(first, second);
    set_window_wm_class(conn, second, "test", "FullscreenOriginal");
    observe_title_after_events(conn, first);
    expect_owner(second, first);

    // Restoring a minimized fullscreen client is an interaction, too.
    send_client_message(conn, first, state, 1, minimized);
    observe_title_after_events(conn, first);
    REQUIRE(has_state(conn, first, minimized));
    send_client_message(conn, first, state, 0, minimized);
    observe_title_after_events(conn, first);
    expect_owner(first, second);
    CHECK_FALSE(has_state(conn, first, minimized));
    destroy_window(conn, first);
    destroy_window(conn, second);
}

TEST_CASE(
    "Integration: restart preserves fullscreen claims across workspace visibility",
    "[integration][restart][fullscreen]"
)
{
    bool hidden = GENERATE(false, true);
    bool failed_exec = GENERATE(false, true);
    CAPTURE(hidden, failed_exec);
    auto env = TestEnvironment::create("[workspaces]\ncount=2\n");
    REQUIRE(env);
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto first = create_window(conn, 10, 10, 320, 240);
    auto second = create_window(conn, 20, 20, 320, 240);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, timeout));
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, timeout));
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    // The older window claims last: creation/adoption order cannot substitute
    // for the actual ownership history.
    send_client_message(conn, second, state, 1, fullscreen);
    send_client_message(conn, first, state, 1, fullscreen);
    observe_title_after_events(conn, first);
    auto expect_owner = [&](xcb_window_t owner, xcb_window_t suppressed)
    {
        auto shown = get_window_geometry(conn, owner);
        auto parked = get_window_geometry(conn, suppressed);
        REQUIRE(shown);
        REQUIRE(parked);
        CHECK(shown->x == 0);
        CHECK(shown->width == conn.screen()->width_in_pixels);
        CHECK(parked->x < -1000);
        CHECK(has_state(conn, owner, fullscreen));
        CHECK(has_state(conn, suppressed, fullscreen));
    };
    expect_owner(first, second);
    if (hidden)
        ipc_ok(*socket, "workspace switch 1");
    auto instance = wm_instance(conn);
    REQUIRE(instance);
    ipc_ok(*socket, failed_exec ? "exec /definitely/missing/lwm-restart" : "restart");
    REQUIRE(wait_for_wm_restart(conn, timeout, *instance));
    ipc_ok(*socket, "workspace switch 0");
    expect_owner(first, second);
    // New claims must outrank every restored claim, then withdrawing one must
    // reveal the remaining candidate rather than relying on a saved winner.
    send_client_message(conn, second, state, 1, fullscreen);
    observe_title_after_events(conn, first);
    expect_owner(second, first);
    send_client_message(conn, second, state, 0, fullscreen);
    observe_title_after_events(conn, first);
    auto restored = get_window_geometry(conn, first);
    REQUIRE(restored);
    CHECK(restored->x == 0);
    destroy_window(conn, first);
    destroy_window(conn, second);
}
