#include "restart_handoff.hpp"
#include <X11/keysym.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
#include <xcb/xcb_ewmh.h>
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
nlohmann::json query(std::string const& command)
{
    auto reply = send_ipc_command(command);
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
names = ["1", "2"]
[[rules]]
match = { class = "FloatingOutput" }
apply = { floating = true, monitor = "DUMMY1" }
)");
    REQUIRE(env);
    REQUIRE(env->x11_env.owns_display());
    RestoreOutputs restore;
    auto& conn = env->conn;
    auto command = [&](std::string text)
    {
        auto result = send_ipc_command(text);
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
    command("window focus " + std::to_string(a));
    auto original_width = width(conn, a);
    REQUIRE(original_width > 700);
    command("workspace switch 1");
    command("layout set monocle");

    randr({ "--addmode", "DUMMY1", "1280x720" });
    randr({ "--output", "DUMMY1", "--mode", "1280x720", "--right-of", "DUMMY0" });
    bool two_outputs =
        wait_for_condition([&] { return query("workspace list")["monitors"].size() == 2; }, timeout);
    INFO(query("workspace list").dump());
    INFO(env->wm.diagnostics());
    auto outputs = run_command("/usr/bin/xrandr", { "--query" });
    INFO((outputs ? outputs->stdout_text : "xrandr failed"));
    REQUIRE(two_outputs);
    check_external_geometry();
    auto workspaces = query("workspace list");
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
        auto windows = query("window list");
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
    command("window focus " + std::to_string(a));
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
        [&] { return query("workspace list")["monitors"].size() == 1 && monitor_for(a) == 0; },
        timeout
    ));
    REQUIRE(wait_for_active_window(conn, a, timeout));
    CHECK(width(conn, a) == 1280);
    check_external_geometry();
    randr({ "--output", "DUMMY0", "--mode", "1280x720", "--right-of", "DUMMY1" });
    REQUIRE(wait_for_condition([&] { return query("workspace list")["monitors"].size() == 2; }, timeout));
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
names = ["1", "2"]
[focus]
warp_cursor_on_monitor_change = false
[binds]
"F9" = "window to-monitor next"
"F10" = "window to-workspace 1"
)");
    REQUIRE(env);
    RestoreOutputs restore;
    auto& conn = env->conn;
    randr({ "--addmode", "DUMMY1", "1280x720" });
    randr({ "--output", "DUMMY1", "--mode", "1280x720", "--right-of", "DUMMY0" });
    auto two_outputs =
        wait_for_condition([&] { return query("workspace list")["monitors"].size() == 2; }, timeout);
    INFO(query("workspace list").dump());
    INFO(env->wm.diagnostics());
    auto outputs = run_command("/usr/bin/xrandr", { "--query" });
    INFO((outputs ? outputs->stdout_text : "xrandr failed"));
    REQUIRE(two_outputs);
    auto command = [&](std::string text)
    {
        auto reply = send_ipc_command(text);
        REQUIRE(reply);
        REQUIRE(reply->starts_with("ok"));
    };
    auto rectangle = [&](xcb_window_t window)
    {
        auto* reply = xcb_get_geometry_reply(conn.get(), xcb_get_geometry(conn.get(), window), nullptr);
        REQUIRE(reply);
        std::array<int, 4> result{ reply->x, reply->y, reply->width, reply->height };
        free(reply);
        return result;
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
            auto snapshot = query("window list");
            for (auto const& client : snapshot["windows"])
                if (client["id"] == window)
                    return client;
            return { };
        };
        auto before = rectangle(window);
        auto source = location()["monitor"].get<size_t>();
        auto destination = 1 - source;
        REQUIRE(send_key(conn, XK_F9));
        REQUIRE(wait_for_condition([&] { return location()["monitor"] == destination; }, timeout));
        REQUIRE(wait_for_active_window(conn, window, timeout));
        CHECK(query("workspace list")["focused_monitor"] == destination);
        auto moved = rectangle(window);
        if (floating)
            REQUIRE(
                moved
                == std::array<int, 4>{ static_cast<int>(destination) * 1280 + (1280 - before[2] - 4) / 2,
                                       (720 - before[3] - 4) / 2,
                                       before[2],
                                       before[3] }
            );
        auto workspace = location()["workspace"].get<size_t>();
        command("workspace switch 0");
        command("window focus " + std::to_string(window));
        // Both destinations start on workspace 0; moving away clears active focus.
        REQUIRE(workspace == 0);
        REQUIRE(send_key(conn, XK_F10));
        REQUIRE(wait_for_condition([&] { return location()["workspace"] == 1; }, timeout));
        REQUIRE(wait_for_condition([&] { return query("window list")["focused"] != window; }, timeout));
        command("workspace switch 1");
        REQUIRE(wait_for_active_window(conn, window, timeout));
        REQUIRE(rectangle(window) == moved);
        command("workspace switch 0");
        destroy_window(conn, window);
    }
}

TEST_CASE(
    "Integration: partial struts reserve root regions independently of dock position",
    "[integration][multioutput][.multioutput]"
)
{
    auto* server = std::getenv("LWM_TEST_XSERVER");
    if (!server || std::strcmp(server, "Xorg") != 0)
        SKIP("Select the owned Xorg dummy server for multi-output coverage");
    auto env = TestEnvironment::create("[workspaces]\nnames = [\"1\"]\n");
    REQUIRE(env);
    REQUIRE(env->x11_env.owns_display());
    RestoreOutputs restore;
    randr({ "--addmode", "DUMMY1", "1280x720" });
    randr({ "--output", "DUMMY1", "--mode", "1280x720", "--pos", "1280x0" });
    auto& conn = env->conn;
    REQUIRE(wait_for_condition([&] { return query("workspace list")["monitors"].size() == 2; }, timeout));
    auto workarea = intern_atom(conn.get(), "_NET_WORKAREA");
    auto partial = intern_atom(conn.get(), "_NET_WM_STRUT_PARTIAL");
    auto legacy = intern_atom(conn.get(), "_NET_WM_STRUT");
    auto check = [&](std::vector<uint32_t> expected)
    {
        REQUIRE(wait_for_condition(
            [&]
            {
                auto actual = read_property32(conn.get(), conn.root(), workarea, XCB_ATOM_CARDINAL);
                return actual && *actual == expected;
            },
            timeout
        ));
    };
    check({ 0, 0, 1280, 720, 1280, 0, 1280, 720 });
    auto dock = create_window(conn, 0, 0, 100, 40);
    set_window_type(conn, dock, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DOCK"));
    uint32_t old[] = { 0, 0, 25, 0 };
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, dock, legacy, XCB_ATOM_CARDINAL, 32, 4, old);
    uint32_t values[] = { 0, 0, 40, 0, 0, 0, 0, 0, 1280, 2559, 0, 0 };
    auto publish = [&](uint8_t format, uint32_t count)
    {
        xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, dock, partial, XCB_ATOM_CARDINAL, format, count, values);
        xcb_flush(conn.get());
    };
    publish(32, 12);
    map_window(conn, dock);
    check({ 0, 0, 1280, 720, 1280, 40, 1280, 680 });
    values[8] = 0;
    values[9] = 1279;
    publish(32, 12);
    check({ 0, 40, 1280, 680, 1280, 0, 1280, 720 });
    // A complete zero partial strut overrides the nonzero legacy reservation.
    values[2] = 0;
    publish(32, 12);
    check({ 0, 0, 1280, 720, 1280, 0, 1280, 720 });
    // A malformed partial property falls back to the legacy edge-wide reservation.
    publish(8, 48);
    check({ 0, 25, 1280, 695, 1280, 25, 1280, 695 });
    xcb_destroy_window(conn.get(), dock);
    xcb_flush(conn.get());
    check({ 0, 0, 1280, 720, 1280, 0, 1280, 720 });
}

TEST_CASE(
    "Integration: a pointer drag crosses outputs and releases on output removal",
    "[integration][multioutput][.multioutput][drag]"
)
{
    auto* server = std::getenv("LWM_TEST_XSERVER");
    if (!server || std::strcmp(server, "Xorg") != 0)
        SKIP("Select the owned Xorg dummy server for multi-output coverage");
    auto env = TestEnvironment::create();
    REQUIRE(env);
    RestoreOutputs restore;
    auto& conn = env->conn;
    randr({ "--addmode", "DUMMY1", "1280x720" });
    randr({ "--output", "DUMMY1", "--mode", "1280x720", "--right-of", "DUMMY0" });
    REQUIRE(wait_for_condition([&] { return query("workspace list")["monitors"].size() == 2; }, timeout));
    auto window = create_window(conn, 10, 10, 200, 150);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    auto monitor = [&]
    {
        auto snapshot = query("window list");
        for (auto const& client : snapshot["windows"])
            if (client["id"] == window)
                return client["monitor"].get<int>();
        return -1;
    };
    REQUIRE(monitor() == 0);
    send_client_message(conn, window, intern_atom(conn.get(), "_NET_WM_MOVERESIZE"), 100, 100, 8, 1);
    observe_title_after_events(conn, window);
    xcb_test_fake_input(conn.get(), XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, conn.root(), 1500, 200, 0);
    observe_title_after_events(conn, window);
    REQUIRE(monitor() == 1);
    auto grab_status = [&]
    {
        auto* reply = xcb_grab_pointer_reply(
            conn.get(),
            xcb_grab_pointer(
                conn.get(),
                0,
                conn.root(),
                XCB_EVENT_MASK_POINTER_MOTION,
                XCB_GRAB_MODE_ASYNC,
                XCB_GRAB_MODE_ASYNC,
                XCB_NONE,
                XCB_NONE,
                XCB_CURRENT_TIME
            ),
            nullptr
        );
        REQUIRE(reply);
        auto status = reply->status;
        free(reply);
        return status;
    };
    REQUIRE(grab_status() == XCB_GRAB_STATUS_ALREADY_GRABBED);
    randr({ "--output", "DUMMY1", "--off" });
    REQUIRE(wait_for_condition(
        [&] { return query("workspace list")["monitors"].size() == 1 && monitor() == 0; },
        timeout
    ));
    REQUIRE(grab_status() == XCB_GRAB_STATUS_SUCCESS);
    xcb_ungrab_pointer(conn.get(), XCB_CURRENT_TIME);
    xcb_flush(conn.get());
}

TEST_CASE(
    "Integration: tile return slots survive output reorder and expire on removal",
    "[integration][tile-slot][multioutput][.multioutput][restart]"
)
{
    auto* server = std::getenv("LWM_TEST_XSERVER");
    if (!server || std::strcmp(server, "Xorg") != 0)
        SKIP("Select the owned Xorg dummy server for multi-output coverage");
    auto env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    REQUIRE(env);
    RestoreOutputs restore;
    auto& conn = env->conn;
    randr({ "--addmode", "DUMMY1", "1280x720" });
    randr({ "--output", "DUMMY1", "--mode", "1280x720", "--right-of", "DUMMY0" });
    REQUIRE(wait_for_condition([&] { return query("workspace list")["monitors"].size() == 2; }, timeout));
    // Explicit desktop requests make placement independent of pointer position.
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 3; ++i)
    {
        auto window = create_window(conn, 20, 20, 200, 150);
        set_window_desktop(conn, window, 0);
        map_window(conn, window);
        REQUIRE(wait_for_condition(
            [&] { return get_window_property_string(conn.get(), window, intern_atom(conn.get(), "_LWM_WINDOW_CLASS")) == "tiled"; },
            timeout
        ));
        observe_title_after_events(conn, window);
        windows.push_back(window);
    }
    auto a = windows[0], b = windows[1], c = windows[2];
    ipc_ok("window focus " + std::to_string(b));
    ipc_ok("window float");
    bool removed = false;
    SECTION("Reordered outputs retain the workspace slot")
    {
        randr({ "--output", "DUMMY1", "--pos", "0x0", "--output", "DUMMY0", "--pos", "1280x0" });
        REQUIRE(wait_for_condition(
            [&] { return query("workspace list")["monitors"][0]["name"] == "DUMMY1"; }, timeout
        ));
    }
    SECTION("Removed outputs cannot lend their slot to the survivor")
    {
        removed = true;
        randr({ "--output", "DUMMY0", "--off" });
        REQUIRE(wait_for_condition([&] { return query("workspace list")["monitors"].size() == 1; }, timeout));
    }
    // Also carry the resulting slot decision through a real exec handoff.
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    ipc_ok("restart");
    REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    ipc_ok("window focus " + std::to_string(b));
    ipc_ok("window float");
    auto ga = require_window_geometry(conn, a);
    auto gb = require_window_geometry(conn, b);
    auto gc = require_window_geometry(conn, c);
    CHECK(ga.x < gb.x);
    CHECK(gb.x == gc.x);
    CHECK((gb.y > gc.y) == removed);
    CHECK(gb.y != gc.y);
    for (auto window : windows) destroy_window(conn, window);
}

TEST_CASE(
    "Integration: output changes during restart preserve identity and subsequent placement",
    "[integration][restart][multioutput][.multioutput]"
)
{
    auto* server = std::getenv("LWM_TEST_XSERVER");
    if (!server || std::strcmp(server, "Xorg") != 0)
        SKIP("Select the owned Xorg dummy server for multi-output coverage");
    bool removed = GENERATE(false, true);
    CAPTURE(removed);
    auto env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    REQUIRE(env);
    RestoreOutputs restore;
    auto& conn = env->conn;
    randr({ "--addmode", "DUMMY1", "1280x720" });
    randr({ "--output", "DUMMY1", "--mode", "1280x720", "--right-of", "DUMMY0" });
    REQUIRE(wait_for_condition([&] { return query("workspace list")["monitors"].size() == 2; }, timeout));
    auto tile = [&](uint32_t desktop)
    {
        auto window = create_window(conn, 20, 20, 200, 150);
        set_window_desktop(conn, window, desktop);
        map_window(conn, window);
        REQUIRE(wait_for_condition(
            [&]
            {
                return get_window_property_string(conn.get(), window, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"))
                    == "tiled";
            },
            timeout
        ));
        return window;
    };
    auto a = tile(1), b = tile(1), c = tile(3), d = tile(3);
    ipc_ok("window focus " + std::to_string(c));
    ipc_ok("ratio set 0.7");
    auto floating = create_window(conn, 40, 40, 250, 180);
    set_window_type(conn, floating, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    set_window_desktop(conn, floating, 1);
    map_window(conn, floating);
    REQUIRE(wait_for_condition(
        [&]
        {
            return get_window_property_string(conn.get(), floating, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"))
                == "floating";
        },
        timeout
    ));
    auto hint = intern_atom(conn.get(), "_NET_WM_FULLSCREEN_MONITORS");
    send_client_message(conn, floating, hint, 0, 1, 0, 1);
    observe_title_after_events(conn, floating);
    REQUIRE(read_property32(conn.get(), floating, hint, XCB_ATOM_CARDINAL) == std::vector<uint32_t>{ 0, 1, 0, 1 });
    ipc_ok("window focus " + std::to_string(b));
    ipc_ok("window swap prev"); // Saved order is b, a, unlike adoption order.
    ipc_ok("ratio set 0.3");
    ipc_ok("layout set monocle");
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    PausedRestart restart(env->wm);
    if (removed)
        randr({ "--output", "DUMMY0", "--off", "--output", "DUMMY1", "--pos", "0x0" });
    else
        randr({ "--output", "DUMMY1", "--pos", "0x0", "--output", "DUMMY0", "--pos", "1280x0" });
    // The successor must know even this newly arrived dock before fitting the
    // saved floating client into the replacement workarea.
    auto dock = create_window(conn, 0, 0, 200, 80);
    set_window_type(conn, dock, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DOCK"));
    uint32_t strut[] = { 0, 0, 80, 0 };
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        dock,
        intern_atom(conn.get(), "_NET_WM_STRUT"),
        XCB_ATOM_CARDINAL,
        32,
        4,
        strut
    );
    map_window(conn, dock);
    auto newcomer = create_window(conn, 20, 20, 200, 150);
    map_window(conn, newcomer);
    REQUIRE(get_window_geometry(conn, newcomer));
    restart.resume();
    REQUIRE(wait_for_wm_restart(conn, std::chrono::seconds(5), *previous));
    auto workspaces = query("workspace list");
    size_t target = removed ? 0 : 1;
    REQUIRE(workspaces["monitors"].size() == (removed ? 1 : 2));
    CHECK(workspaces["focused_monitor"] == target);
    CHECK(workspaces["monitors"][0]["name"] == "DUMMY1");
    CHECK(workspaces["monitors"][0]["current_workspace"] == 1);
    CHECK(workspaces["monitors"][0]["workspaces"][1]["layout"] == "master-stack");
    if (!removed)
    {
        CHECK(workspaces["monitors"][1]["name"] == "DUMMY0");
        CHECK(workspaces["monitors"][1]["current_workspace"] == 1);
        CHECK(workspaces["monitors"][1]["workspaces"][1]["layout"] == "monocle");
    }
    auto monitor_for = [&](xcb_window_t id)
    {
        auto windows = query("window list");
        for (auto const& window : windows["windows"])
            if (window["id"] == id)
            {
                CHECK(window["workspace"] == 1);
                return window["monitor"].get<size_t>();
            }
        FAIL("Missing window " << id);
        return size_t(-1);
    };
    for (auto window : { a, b, floating, newcomer }) CHECK(monitor_for(window) == target);
    CHECK(monitor_for(c) == 0);
    CHECK(monitor_for(d) == 0);
    REQUIRE(wait_for_active_window(conn, b, timeout));
    CHECK(read_property32(conn.get(), floating, hint, XCB_ATOM_CARDINAL).value_or(std::vector<uint32_t>{ }).empty());
    auto rectangle = require_window_geometry(conn, floating);
    if (removed)
    {
        // The frame, including the default 2px border, is centered.
        CHECK(rectangle.x == (1280 - rectangle.width - 4) / 2);
        CHECK(rectangle.y == 80 + (720 - 80 - rectangle.height - 4) / 2);
    }
    else
        CHECK(rectangle.x >= 1280);

    // Subsequent commands must target the restored output and use its saved
    // ratios/order, with displaced tiles appended after surviving members.
    ipc_ok("layout set master-stack");
    auto master = require_window_geometry(conn, removed ? c : b);
    CHECK((master.width > 700) == removed); // Survivor's 0.7 versus source's 0.3.
    auto ga = require_window_geometry(conn, a), gb = require_window_geometry(conn, b);
    auto gn = require_window_geometry(conn, newcomer);
    if (removed)
    {
        auto gd = require_window_geometry(conn, d);
        CHECK(master.x < gd.x);
        CHECK(gd.y < gb.y);
        CHECK(gb.y < ga.y);
    }
    else
        CHECK(gb.x < ga.x);
    CHECK(ga.y < gn.y);
    ipc_ok("workspace switch 0");
    REQUIRE(wait_for_condition([&] { return is_hidden_offscreen(conn, b); }, timeout));
    ipc_ok("workspace toggle");
    REQUIRE(wait_for_active_window(conn, b, timeout));
    if (removed)
    {
        randr({ "--output", "DUMMY0", "--mode", "1280x720", "--right-of", "DUMMY1" });
        REQUIRE(wait_for_condition([&] { return query("workspace list")["monitors"].size() == 2; }, timeout));
        CHECK(monitor_for(a) == 0);
        CHECK(monitor_for(floating) == 0);
        CHECK(query("workspace list")["monitors"][1]["current_workspace"] == 0);
    }
    for (auto window : { a, b, c, d, floating, newcomer, dock }) destroy_window(conn, window);
}

TEST_CASE(
    "Integration: named and pooled recall bring a visible window to the focused output",
    "[integration][scratchpad][multioutput][.multioutput]"
)
{
    auto* server = std::getenv("LWM_TEST_XSERVER");
    if (!server || std::strcmp(server, "Xorg") != 0)
        SKIP("Select the owned Xorg dummy server for multi-output coverage");
    auto kind = GENERATE("named", "tiled pool", "floating pool");
    bool named = std::string_view(kind) == "named";
    CAPTURE(kind);
    auto env = TestEnvironment::create(R"(
[workspaces]
names = ["1", "2"]
[[scratchpads]]
name = "recall"
spawn = ["/bin/true"]
match = { class = "RecallNamed" }
)");
    REQUIRE(env);
    REQUIRE(env->x11_env.owns_display());
    RestoreOutputs restore;
    randr({ "--addmode", "DUMMY1", "1280x720" });
    randr({ "--output", "DUMMY1", "--mode", "1280x720", "--pos", "1280x0" });
    auto& conn = env->conn;
    REQUIRE(wait_for_condition([&] { return query("workspace list")["monitors"].size() == 2; }, timeout));
    auto desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    send_client_message(conn, conn.root(), desktop, 0);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), desktop, 0, timeout));
    auto window = create_window(conn, 10, 10, 240, 160);
    if (named)
        set_window_wm_class(conn, window, "recall", "RecallNamed");
    map_window(conn, window);
    if (named)
        REQUIRE(wait_for_condition([&] { return is_hidden_offscreen(conn, window); }, timeout));
    else
    {
        REQUIRE(wait_for_active_window(conn, window, timeout));
        if (std::string_view(kind) == "floating pool")
            ipc_ok("window float");
        ipc_ok("scratchpad stash");
    }
    std::string recall = named ? "scratchpad toggle recall" : "scratchpad cycle";
    ipc_ok(recall);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    ipc_ok("monitor focus next");
    ipc_ok("workspace switch 1");
    REQUIRE_FALSE(is_hidden_offscreen(conn, window));
    ipc_ok(recall);
    INFO(ipc_ok("state"));
    REQUIRE(wait_for_active_window(conn, window, timeout));
    REQUIRE(wait_for_property_cardinal(conn.get(), window, intern_atom(conn.get(), "_NET_WM_DESKTOP"), 3, timeout));
    auto geometry = get_window_geometry(conn, window);
    REQUIRE(geometry);
    REQUIRE(geometry->x >= 1280);
    destroy_window(conn, window);
}
