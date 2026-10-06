#include "state_watch.hpp"
#include "wm_observations.hpp"
#include <X11/Xlib.h>
#include <xcb/xtest.h>

using namespace lwm::test;

namespace {
constexpr auto kTimeout = std::chrono::seconds(2);

nlohmann::json const& only_window(nlohmann::json const& state) { return state.at("windows").at("windows").at(0); }
bool near(nlohmann::json const& value, double expected) { return std::abs(value.get<double>() - expected) < 1e-9; }
}

TEST_CASE("Integration: watch prints the attached state, then only changed states", "[integration][watch]")
{
    auto env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 10, 10, 200, 150);
    set_window_wm_class(conn, window, "escaped-instance", "escaped-class");
    std::string title = "quote \" and slash \\ line\n tab\t";
    set_window_title(conn, window, title);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    Watcher watcher;
    auto attached = watcher.state();
    CHECK(attached == nlohmann::json::parse(ipc_ok("state").substr(3)));
    CHECK(attached.at("windows").at("focused") == window);
    CHECK(only_window(attached).at("class") == "escaped-class");
    CHECK(only_window(attached).at("title") == title);
    CHECK(attached.at("workspaces").contains("monitors"));
    CHECK(attached.at("scratchpads").contains("named"));

    // Queries and no-op actions leave the exposed state unchanged.
    ipc_ok("window list");
    ipc_ok("workspace switch 0");
    ipc_ok("ratio reset");
    CHECK(watcher.quiet());
    ipc_ok("workspace switch 1");
    CHECK(watcher.state().at("workspaces").at("monitors").at(0).at("current_workspace") == 1);
    ipc_ok("workspace switch 1");
    CHECK(watcher.quiet());
    ipc_ok("workspace switch 0");
    CHECK(watcher.state().at("workspaces").at("monitors").at(0).at("current_workspace") == 0);

    // Metadata alone changes the state, even when no rule matches.
    set_window_title(conn, window, "watch-new-title");
    CHECK(only_window(watcher.state()).at("title") == "watch-new-title");
    CHECK(watcher.quiet());
    destroy_window(conn, window);
}

TEST_CASE("Integration: watch reports layout changes from bindings, IPC and split drags", "[integration][watch][actions]")
{
    auto env = TestEnvironment::create(
        "[layout]\nmin_ratio = 0.1\n[appearance]\npadding = 10\nborder_width = 1\n"
        "[[binds]]\nkey = \"super+l\"\naction = 'ratio adjust 0.05'\n"
    );
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto first = create_window(conn, 10, 10, 200, 200);
    auto second = create_window(conn, 10, 10, 200, 200);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, kTimeout));
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, kTimeout));
    Watcher watcher;
    auto workspace = [&] { return watcher.state().at("workspaces").at("monitors").at(0).at("workspaces").at(0); };
    CHECK(near(workspace().at("ratio"), 0.5));

    // A pointer drag of the split publishes its final ratio once, on release.
    auto left = get_window_geometry(conn, first);
    auto right = get_window_geometry(conn, second);
    REQUIRE(left);
    REQUIRE(right);
    if (left->x > right->x)
        std::swap(left, right);
    int16_t x = static_cast<int16_t>((left->x + left->width + right->x) / 2);
    int16_t y = static_cast<int16_t>(left->y + left->height / 2);
    send_pointer_event(conn, XCB_BUTTON_PRESS, x, y);
    send_pointer_event(conn, XCB_MOTION_NOTIFY, static_cast<int16_t>(x + 30), y);
    send_pointer_event(conn, XCB_MOTION_NOTIFY, static_cast<int16_t>(x + 60), y);
    send_pointer_event(conn, XCB_BUTTON_RELEASE, static_cast<int16_t>(x + 60), y);
    CHECK(workspace().at("ratio").get<double>() > 0.52);
    ipc_ok("ratio reset");
    CHECK(near(workspace().at("ratio"), 0.5));

    if (extension_available(conn, &xcb_test_id))
    {
        REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("l")));
        CHECK(near(workspace().at("ratio"), 0.55));
    }
    else
        ipc_ok("ratio adjust 0.05");
    ipc_ok("ratio adjust 0.05");
    CHECK(near(workspace().at("ratio"), 0.6));
    ipc_ok("ratio set 0.6123456789");
    CHECK(workspace().at("ratio") == 0.6123456789);
    ipc_ok("layout set monocle");
    CHECK(workspace().at("layout") == "monocle");
    CHECK(watcher.quiet());
    destroy_window(conn, second);
    destroy_window(conn, first);
}

TEST_CASE("Integration: watch exits normally when its consumer closes", "[integration][watch]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    Watcher watcher;
    REQUIRE(watcher.line());
    watcher.output.reset();
    auto window = create_window(conn, 40, 40, 200, 150);
    map_window(conn, window);
    int status = 0;
    REQUIRE(wait_for_condition(
        [&]
        {
            if (waitpid(watcher.pid, &status, WNOHANG) != watcher.pid)
                return false;
            watcher.pid = -1;
            return true;
        },
        kTimeout
    ));
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    destroy_window(conn, window);
}

TEST_CASE("Integration: watch follows an exec restart", "[integration][watch][restart]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto window = create_window(conn, 30, 40, 200, 150);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    Watcher watcher;
    CHECK(watcher.state().at("windows").at("focused") == window);
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    ipc_ok("restart");
    REQUIRE(wait_for_wm_restart(conn, kTimeout, *previous));
    // The successor's state is identical, so a later change proves reattachment.
    set_window_title(conn, window, "after-restart");
    nlohmann::json state;
    REQUIRE(wait_for_condition(
        [&]
        {
            auto line = watcher.line(std::chrono::milliseconds(100));
            if (line)
                state = nlohmann::json::parse(*line);
            return !state.is_null() && only_window(state).at("title") == "after-restart";
        },
        kTimeout
    ));
    destroy_window(conn, window);
}

TEST_CASE("Integration: a state too large for one X request is withheld rather than fatal", "[integration][watch]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    // Control bytes expand sixfold as JSON escapes: maximal titles and classes add
    // about 70 KB of state per window, enough to exceed the server's request limit.
    std::string text(4096, '\x01');
    auto count = xcb_get_maximum_request_length(conn.get()) * 4 / 70000 + 20;
    std::vector<xcb_window_t> windows;
    for (size_t i = 0; i < count; ++i)
    {
        windows.push_back(create_window(conn, 10, 10, 50, 50));
        set_window_title(conn, windows.back(), text);
        set_window_wm_class(conn, windows.back(), text, text);
        map_window(conn, windows.back());
    }
    REQUIRE(wait_for_active_window(conn, windows.back(), std::chrono::seconds(40))); // Slow under sanitizers
    CHECK(send_ipc_command("state", std::chrono::seconds(10)) == "error response too large");
    CHECK(send_ipc_command("ping") == "ok pong");
    CHECK(env->wm.running());
    for (auto window : windows) destroy_window(conn, window);
}
