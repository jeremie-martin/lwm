#include "x11_test_harness.hpp"
#include <X11/keysym.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xtest.h>
using namespace lwm::test;
namespace {
constexpr auto timeout = std::chrono::seconds(3);
void randr(std::vector<std::string> args)
{
    auto result = run_command("/usr/bin/xrandr", args);
    REQUIRE(result);
    INFO(result->stderr_text);
    REQUIRE(result->exit_code == 0);
    // The dummy driver publishes newly connected outputs when probed.
    auto probe = run_command("/usr/bin/xrandr", { "--query" });
    REQUIRE(probe);
    REQUIRE(probe->exit_code == 0);
}
struct RestoreOutputs
{
    ~RestoreOutputs()
    {
        run_command(
            "/usr/bin/xrandr",
            { "--output", "DUMMY0", "--mode", "1280x720", "--pos", "0x0", "--output", "DUMMY1", "--off" }
        );
    }
};
nlohmann::json query(std::string const& socket, std::string const& command)
{
    auto reply = send_ipc_command(socket, command);
    if (!reply || !reply->starts_with("ok "))
        return {};
    return nlohmann::json::parse(reply->substr(3));
}
uint16_t width(X11Connection& conn, xcb_window_t window)
{
    auto* reply = xcb_get_geometry_reply(conn.get(), xcb_get_geometry(conn.get(), window), nullptr);
    auto value = reply ? reply->width : 0;
    free(reply);
    return value;
}
}
TEST_CASE(
    "Integration: output add reorder removal and return preserve surviving workspaces",
    "[integration][multioutput][.multioutput]"
)
{
    auto* server = std::getenv("LWM_TEST_XSERVER");
    if (!server || std::strcmp(server, "Xorg") != 0)
        SKIP("Select the owned Xorg dummy server for multi-output coverage");
    auto env = TestEnvironment::create(R"(
[workspaces]
count = 2
[[rules]]
match = { class = "FloatingOutput" }
apply = { floating = true, monitor_name = "DUMMY1" }
)");
    REQUIRE(env);
    REQUIRE(env->x11_env.owns_display());
    RestoreOutputs restore;
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto command = [&](std::string text)
    {
        auto result = send_ipc_command(*socket, text);
        REQUIRE(result);
        REQUIRE(result->starts_with("ok"));
    };
    auto dock = create_window(conn, 15, 25, 500, 40);
    auto desktop = create_window(conn, 0, 0, 600, 400);
    set_window_type(conn, dock, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DOCK"));
    set_window_type(conn, desktop, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DESKTOP"));
    map_window(conn, dock);
    map_window(conn, desktop);
    auto classification = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    REQUIRE(wait_for_condition(
        [&]
        {
            return get_window_property_string(conn.get(), dock, classification) == "dock"
                && get_window_property_string(conn.get(), desktop, classification) == "desktop";
        },
        timeout
    ));
    auto check_external_geometry = [&]
    {
        CHECK(width(conn, dock) == 500);
        CHECK(width(conn, desktop) == 600);
    };
    auto a = create_window(conn, 10, 10, 200, 150);
    auto b = create_window(conn, 10, 10, 200, 150);
    map_window(conn, a);
    REQUIRE(wait_for_active_window(conn, a, timeout));
    map_window(conn, b);
    REQUIRE(wait_for_active_window(conn, b, timeout));
    command("ratio set 0.7");
    command("focus window=" + std::to_string(a));
    auto original_width = width(conn, a);
    REQUIRE(original_width > 700);
    command("workspace switch 1");
    command("layout set monocle");

    randr({ "--addmode", "DUMMY1", "1280x720" });
    randr({ "--output", "DUMMY1", "--mode", "1280x720", "--right-of", "DUMMY0" });
    bool two_outputs =
        wait_for_condition([&] { return query(*socket, "workspace list")["monitors"].size() == 2; }, timeout);
    INFO(query(*socket, "workspace list").dump());
    INFO(env->wm.diagnostics());
    auto outputs = run_command("/usr/bin/xrandr", { "--query" });
    INFO((outputs ? outputs->stdout_text : "xrandr failed"));
    REQUIRE(two_outputs);
    check_external_geometry();
    auto workspaces = query(*socket, "workspace list");
    CHECK(workspaces["monitors"][0]["current_workspace"] == 1);
    CHECK(workspaces["monitors"][0]["workspaces"][1]["layout"] == "monocle");
    command("workspace switch 0");
    REQUIRE(wait_for_active_window(conn, a, timeout));
    CHECK(width(conn, a) == original_width);
    check_external_geometry();

    auto floating = create_window(conn, 1300, 20, 250, 180);
    set_window_wm_class(conn, floating, "floating", "FloatingOutput");
    map_window(conn, floating);
    auto monitor_for = [&](xcb_window_t id) -> int
    {
        auto windows = query(*socket, "window list");
        if (windows.contains("windows"))
            for (auto const& window : windows["windows"])
                if (window["id"] == id)
                    return window["monitor"].get<int>();
        return -1;
    };
    REQUIRE(wait_for_condition([&] { return monitor_for(floating) == 1; }, timeout));
    randr({ "--output", "DUMMY1", "--pos", "0x0", "--output", "DUMMY0", "--pos", "1280x0" });
    REQUIRE(wait_for_condition([&] { return monitor_for(a) == 1 && monitor_for(floating) == 0; }, timeout));
    CHECK(width(conn, a) == original_width);
    check_external_geometry();
    command("focus window=" + std::to_string(a));
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_client_message_event_t event{};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.window = a;
    event.type = state;
    event.format = 32;
    event.data.data32[0] = 1;
    event.data.data32[1] = fullscreen;
    xcb_send_event(
        conn.get(),
        false,
        conn.root(),
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
        reinterpret_cast<char*>(&event)
    );
    xcb_flush(conn.get());
    REQUIRE(wait_for_condition([&] { return width(conn, a) == 1280; }, timeout));
    randr({ "--output", "DUMMY0", "--off" });
    REQUIRE(wait_for_condition(
        [&] { return query(*socket, "workspace list")["monitors"].size() == 1 && monitor_for(a) == 0; },
        timeout
    ));
    REQUIRE(wait_for_active_window(conn, a, timeout));
    CHECK(width(conn, a) == 1280);
    check_external_geometry();
    randr({ "--output", "DUMMY0", "--mode", "1280x720", "--right-of", "DUMMY1" });
    REQUIRE(wait_for_condition([&] { return query(*socket, "workspace list")["monitors"].size() == 2; }, timeout));
    CHECK(monitor_for(a) == 0); // Returning outputs do not reclaim relocated clients.
    CHECK(monitor_for(floating) == 0);
    check_external_geometry();
    destroy_window(conn, floating);
    destroy_window(conn, b);
    destroy_window(conn, a);
    destroy_window(conn, dock);
    destroy_window(conn, desktop);
}

TEST_CASE(
    "Integration: moving tiled and floating clients shares monitor and workspace focus behavior",
    "[integration][multioutput][.multioutput]"
)
{
    auto* server = std::getenv("LWM_TEST_XSERVER");
    if (!server || std::strcmp(server, "Xorg") != 0)
        SKIP("Select the owned Xorg dummy server for multi-output coverage");
    auto env = TestEnvironment::create(R"(
[workspaces]
count = 2
[focus]
warp_cursor_on_monitor_change = false
[[binds]]
key = "F9"
move_to_monitor = 1
[[binds]]
key = "F10"
move_to_workspace = 1
)");
    REQUIRE(env);
    RestoreOutputs restore;
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    randr({ "--addmode", "DUMMY1", "1280x720" });
    randr({ "--output", "DUMMY1", "--mode", "1280x720", "--right-of", "DUMMY0" });
    auto two_outputs =
        wait_for_condition([&] { return query(*socket, "workspace list")["monitors"].size() == 2; }, timeout);
    INFO(query(*socket, "workspace list").dump());
    INFO(env->wm.diagnostics());
    auto outputs = run_command("/usr/bin/xrandr", { "--query" });
    INFO((outputs ? outputs->stdout_text : "xrandr failed"));
    REQUIRE(two_outputs);
    auto press = [&](xcb_keysym_t symbol)
    {
        auto* symbols = xcb_key_symbols_alloc(conn.get());
        REQUIRE(symbols);
        auto* codes = xcb_key_symbols_get_keycode(symbols, symbol);
        REQUIRE(codes);
        auto code = codes[0];
        free(codes);
        xcb_key_symbols_free(symbols);
        REQUIRE(code != XCB_NO_SYMBOL);
        xcb_test_fake_input(conn.get(), XCB_KEY_PRESS, code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
        xcb_test_fake_input(conn.get(), XCB_KEY_RELEASE, code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
        xcb_flush(conn.get());
    };
    auto command = [&](std::string text)
    {
        auto reply = send_ipc_command(*socket, text);
        REQUIRE(reply);
        REQUIRE(reply->starts_with("ok"));
    };
    for (bool floating : { false, true })
    {
        CAPTURE(floating);
        auto window = create_window(conn, 50, 60, 250, 180);
        if (floating)
            set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
        map_window(conn, window);
        REQUIRE(wait_for_active_window(conn, window, timeout));
        auto location = [&]() -> nlohmann::json
        {
            auto snapshot = query(*socket, "window list");
            for (auto const& client : snapshot["windows"])
                if (client["id"] == window)
                    return client;
            return {};
        };
        auto source = location()["monitor"].get<size_t>();
        auto destination = 1 - source;
        press(XK_F9);
        REQUIRE(wait_for_condition([&] { return location()["monitor"] == destination; }, timeout));
        REQUIRE(wait_for_active_window(conn, window, timeout));
        CHECK(query(*socket, "workspace list")["focused_monitor"] == destination);
        auto workspace = location()["workspace"].get<size_t>();
        command("workspace switch 0");
        command("focus window=" + std::to_string(window));
        // Both destinations start on workspace 0; moving away clears active focus.
        REQUIRE(workspace == 0);
        press(XK_F10);
        REQUIRE(wait_for_condition([&] { return location()["workspace"] == 1; }, timeout));
        REQUIRE(wait_for_condition([&] { return query(*socket, "window list")["focused"] != window; }, timeout));
        command("workspace switch 1");
        REQUIRE(wait_for_active_window(conn, window, timeout));
        command("workspace switch 0");
        destroy_window(conn, window);
    }
}
