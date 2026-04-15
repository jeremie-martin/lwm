#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <sys/socket.h>
#include <sys/un.h>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

struct WindowGeometry
{
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;

    bool operator==(WindowGeometry const&) const = default;
};

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
    };
    free(reply);
    return result;
}

bool is_hidden_offscreen(X11Connection& conn, xcb_window_t window)
{
    auto geometry = get_window_geometry(conn, window);
    return geometry.has_value() && geometry->x < 0;
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
            auto geometry = get_window_geometry(conn, window);
            return geometry.has_value() && geometry->x == x && geometry->y == y && geometry->width == width
                && geometry->height == height;
        },
        kTimeout
    );
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
        kTimeout
    );
    if (!ready)
        return std::nullopt;

    return get_window_property_string(conn.get(), conn.root(), socket_atom);
}

std::optional<std::string> send_ipc_command(std::string const& socket_path, std::string const& command)
{
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path))
        return std::nullopt;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return std::nullopt;

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        close(fd);
        return std::nullopt;
    }

    std::string request = command;
    request.push_back('\n');
    if (send(fd, request.data(), request.size(), 0) < 0)
    {
        close(fd);
        return std::nullopt;
    }

    shutdown(fd, SHUT_WR);

    std::string response;
    char buffer[1024];
    while (true)
    {
        ssize_t bytes_read = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes_read < 0)
        {
            close(fd);
            return std::nullopt;
        }
        if (bytes_read == 0)
            break;
        response.append(buffer, static_cast<size_t>(bytes_read));
    }

    close(fd);

    while (!response.empty() && (response.back() == '\n' || response.back() == '\r' || response.back() == ' '))
        response.pop_back();

    return response;
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

std::string scratchpad_match_config()
{
    return R"(
[workspaces]
count = 1
names = ["1"]

[[scratchpads]]
name = "terminal"
command = "/bin/true"
class = "ScratchpadClass"
instance = "scratchpad-instance"
width = 0.8
height = 0.6
)";
}

std::string title_scratchpad_match_config()
{
    return R"(
[workspaces]
count = 1
names = ["1"]

[[scratchpads]]
name = "terminal"
command = "/bin/true"
class = "ScratchpadClass"
title = "dropdown"
width = 0.8
height = 0.6
)";
}

} // namespace

TEST_CASE("Integration: named scratchpads match WM_CLASS class and instance in the documented order", "[integration][scratchpad]")
{
    auto test_env = TestEnvironment::create(scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t window = create_window(conn, 10, 10, 240, 160);
    set_window_wm_class(conn, window, "scratchpad-instance", "ScratchpadClass");
    map_window(conn, window);

    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, window); }, kTimeout));

    destroy_window(conn, window);
}

TEST_CASE("Integration: named scratchpads can finish a pending launch after a late title update", "[integration][scratchpad]")
{
    auto test_env = TestEnvironment::create(title_scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto socket_path = wait_for_ipc_socket_path(conn);
    REQUIRE(socket_path.has_value());
    auto toggle_result = send_ipc_command(*socket_path, "scratchpad toggle terminal");
    REQUIRE(toggle_result.has_value());
    REQUIRE(*toggle_result == "ok");

    xcb_window_t window = create_window(conn, 10, 10, 240, 160);
    set_window_wm_class(conn, window, "scratchpad-instance", "ScratchpadClass");
    map_window(conn, window);

    REQUIRE(wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, window);
            return geometry.has_value() && geometry->x >= 0;
        },
        kTimeout
    ));

    set_window_title(conn, window, "dropdown");
    REQUIRE(wait_for_window_geometry(conn, window, 128, 144, 1024, 432));

    destroy_window(conn, window);
}

TEST_CASE("Integration: scratchpad cycle keeps pooled windows in rotation", "[integration][scratchpad]")
{
    auto test_env = TestEnvironment::create(scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto socket_path = wait_for_ipc_socket_path(conn);
    REQUIRE(socket_path.has_value());

    xcb_window_t first = create_window(conn, 10, 10, 300, 220);
    set_window_wm_class(conn, first, "pool-first", "PoolWindow");
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, kTimeout));

    xcb_window_t second = create_window(conn, 40, 40, 320, 240);
    set_window_wm_class(conn, second, "pool-second", "PoolWindow");
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, kTimeout));

    auto stash_second = send_ipc_command(*socket_path, "scratchpad stash");
    REQUIRE(stash_second.has_value());
    REQUIRE(*stash_second == "ok");
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, second); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, first, kTimeout));

    auto stash_first = send_ipc_command(*socket_path, "scratchpad stash");
    REQUIRE(stash_first.has_value());
    REQUIRE(*stash_first == "ok");
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, first); }, kTimeout));

    auto first_cycle = send_ipc_command(*socket_path, "scratchpad cycle");
    REQUIRE(first_cycle.has_value());
    REQUIRE(*first_cycle == "ok");
    REQUIRE(wait_for_active_window(conn, first, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, first); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, second); }, kTimeout));

    auto second_cycle = send_ipc_command(*socket_path, "scratchpad cycle");
    REQUIRE(second_cycle.has_value());
    REQUIRE(*second_cycle == "ok");
    REQUIRE(wait_for_active_window(conn, second, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, first); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, second); }, kTimeout));

    auto third_cycle = send_ipc_command(*socket_path, "scratchpad cycle");
    REQUIRE(third_cycle.has_value());
    REQUIRE(*third_cycle == "ok");
    REQUIRE(wait_for_active_window(conn, first, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, first); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, second); }, kTimeout));

    destroy_window(conn, second);
    destroy_window(conn, first);
}
