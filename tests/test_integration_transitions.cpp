#include "lwm/core/types.hpp"
#include "x11_test_harness.hpp"
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
        return {};
    lwm::Geometry result{ reply->x, reply->y, reply->width, reply->height };
    free(reply);
    return result;
}

bool has_state(X11Connection& conn, xcb_window_t window, xcb_atom_t atom)
{
    auto* reply = xcb_get_property_reply(
        conn.get(),
        xcb_get_property(conn.get(), 0, window, intern_atom(conn.get(), "_NET_WM_STATE"), XCB_ATOM_ATOM, 0, 128),
        nullptr
    );
    if (!reply)
        return false;
    auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
    bool found = std::find(atoms, atoms + xcb_get_property_value_length(reply) / 4, atom)
        != atoms + xcb_get_property_value_length(reply) / 4;
    free(reply);
    return found;
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
        xcb_size_hints_t value{};
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
    struct Subscriber
    {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        ~Subscriber()
        {
            if (fd >= 0)
                close(fd);
        }
        std::string line()
        {
            std::string result;
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                pollfd descriptor{ fd, POLLIN, 0 };
                if (poll(&descriptor, 1, 20) <= 0)
                    continue;
                char byte;
                if (recv(fd, &byte, 1, 0) != 1)
                    break;
                if (byte == '\n')
                    return result;
                result += byte;
            }
            return {};
        }
    } subscriber;
    REQUIRE(subscriber.fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    REQUIRE(path->size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path->c_str(), path->size() + 1);
    REQUIRE(connect(subscriber.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    constexpr std::string_view request = "subscribe\n";
    REQUIRE(send(subscriber.fd, request.data(), request.size(), MSG_NOSIGNAL) == request.size());
    REQUIRE(subscriber.line() == "ok subscribed");
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
    pollfd descriptor{ subscriber.fd, POLLIN, 0 };
    CHECK(poll(&descriptor, 1, 30) == 0);
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
        xcb_button_press_event_t event{};
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
    xcb_icccm_wm_hints_t hints{};
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
    auto previous = supporting_wm_window(conn);
    REQUIRE(previous);
    auto restarted = send_ipc_command(*path, "restart");
    REQUIRE(restarted);
    REQUIRE(restarted->starts_with("ok"));
    REQUIRE(wait_for_wm_ready(conn, timeout, *previous));
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
