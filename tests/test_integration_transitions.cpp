#include "ipc_subscription.hpp"
#include "lwm/core/types.hpp"
#include "wm_observations.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <xcb/xcb_icccm.h>

using namespace lwm::test;
namespace {
constexpr auto timeout = std::chrono::seconds(2);

std::optional<lwm::Geometry> geometry(X11Connection& conn, xcb_window_t window)
{
    auto* reply = xcb_get_geometry_reply(conn.get(), xcb_get_geometry(conn.get(), window), nullptr);
    if (!reply)
        return { };
    lwm::Geometry result{ reply->x, reply->y, reply->width, reply->height };
    free(reply);
    return result;
}

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
                    auto rect = geometry(conn, window);
                    return rect && (hidden ? rect->x < -10000 : *rect == lwm::Geometry{ 60, 70, 310, 210 });
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
    REQUIRE(wait_for_condition([&] { return geometry(conn, window) == lwm::Geometry{ 140, 150, 420, 280 }; }, timeout));
    send_client_message(conn, window, state, 1, horizontal);
    REQUIRE(wait_for_condition(
        [&]
        {
            auto rect = geometry(conn, window);
            return rect && rect->width == conn.screen()->width_in_pixels && rect->y == 150 && rect->height == 280;
        },
        timeout
    ));
    send_client_message(conn, window, state, 0, horizontal);
    REQUIRE(wait_for_condition([&] { return geometry(conn, window) == lwm::Geometry{ 140, 150, 420, 280 }; }, timeout));
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: no-op floating configure requests receive a synthetic acknowledgement",
    "[integration][transition][configure]"
)
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 60, 70, 310, 210);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn.get(), window, XCB_CW_EVENT_MASK, &mask);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    auto rect = geometry(conn, window);
    REQUIRE(rect);
    while (auto* event = xcb_poll_for_event(conn.get())) free(event);
    uint32_t values[] = { rect->width, rect->height };
    xcb_configure_window(conn.get(), window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    xcb_flush(conn.get());
    REQUIRE(wait_for_condition(
        [&]
        {
            bool acknowledged = false;
            while (auto* event = xcb_poll_for_event(conn.get()))
            {
                if (event->response_type == (XCB_CONFIGURE_NOTIFY | 0x80))
                {
                    auto const& configure = *reinterpret_cast<xcb_configure_notify_event_t*>(event);
                    acknowledged |= configure.window == window && configure.width == rect->width
                        && configure.height == rect->height;
                }
                free(event);
            }
            return acknowledged;
        },
        timeout
    ));
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
    auto first_geometry = geometry(conn, first);
    auto second_geometry = geometry(conn, second);
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
    auto left = geometry(conn, first);
    auto right = geometry(conn, second);
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
            auto current = geometry(conn, left_window);
            return current && current->width > left->width + 30;
        },
        timeout
    ));
    for (int offset = 41; offset <= 80; ++offset) pointer(XCB_MOTION_NOTIFY, start_x + offset);
    pointer(XCB_BUTTON_RELEASE, start_x + 80);
    pointer(XCB_MOTION_NOTIFY, start_x + 250);
    // This property event follows the entire pointer sequence on the same connection.
    title(conn, first, "resize-finished");
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    REQUIRE(wait_for_condition(
        [&]
        {
            auto reply = send_ipc_command(*path, "window list");
            return reply && reply->find("resize-finished") != std::string::npos;
        },
        timeout
    ));
    auto current = geometry(conn, left_window);
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

TEST_CASE("Integration: maximize requests leave dock and desktop geometry alone", "[integration][transition][geometry]")
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
    auto before = geometry(conn, window);
    REQUIRE(before);
    auto horizontal = intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_HORZ");
    send_client_message(conn, window, intern_atom(conn.get(), "_NET_WM_STATE"), 1, horizontal);
    REQUIRE(wait_for_condition([&] { return has_state(conn, window, horizontal); }, timeout));
    CHECK(geometry(conn, window) == before);
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
    auto rect = geometry(conn, first);
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
    REQUIRE(geometry(conn, second));
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
    auto first_rect = geometry(conn, first);
    auto second_rect = geometry(conn, second);
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
                auto rect = geometry(conn, tiled);
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
