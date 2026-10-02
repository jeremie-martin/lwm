#include "ipc_subscription.hpp"
#include <X11/Xlib.h>
#include <cstdio>
#include <memory>
#include <xcb/xtest.h>

using namespace lwm::test;

namespace {
constexpr auto kTimeout = std::chrono::seconds(2);

// Only the consumer-close contract needs a CLI process. Protocol/content tests
// use Subscriber's acknowledged socket; CLI framing has its own independent peer.
struct CliSubscriber
{
    TestFd output;
    LineReader reader;
    std::unique_ptr<FILE, int (*)(FILE*)> errors{ tmpfile(), fclose };
    pid_t pid = -1;

    explicit CliSubscriber(std::string const& socket)
    {
        REQUIRE(errors);
        REQUIRE(fcntl(fileno(errors.get()), F_SETFD, FD_CLOEXEC) == 0);
        int pipe[2];
        REQUIRE(pipe2(pipe, O_CLOEXEC) == 0);
        output.fd = pipe[0];
        TestFd writer{ pipe[1] };
        auto executable = lwmctl_executable_path();
        pid = fork();
        REQUIRE(pid >= 0);
        if (pid == 0)
        {
            if (dup2(writer.fd, STDOUT_FILENO) < 0 || dup2(fileno(errors.get()), STDERR_FILENO) < 0)
                _exit(126);
            output.reset();
            writer.reset();
            execl(
                executable.c_str(),
                executable.c_str(),
                "--socket",
                socket.c_str(),
                "subscribe",
                "window_map",
                nullptr
            );
            _exit(127);
        }
    }
    ~CliSubscriber()
    {
        if (pid > 0)
        {
            kill(pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
            { }
        }
    }
    bool wait(int& status)
    {
        return wait_for_condition(
            [&]
            {
                if (waitpid(pid, &status, WNOHANG) != pid)
                    return false;
                pid = -1;
                return true;
            },
            kTimeout
        );
    }
};

} // namespace

TEST_CASE("Integration: lwmctl subscribe exits when stdout consumer closes", "[integration][subscribe]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    CliSubscriber child(*socket);

    // The CLI intentionally hides the acknowledgement. Generate real events
    // until one is delivered, rather than guessing when it has subscribed.
    std::vector<xcb_window_t> windows;
    auto deadline = std::chrono::steady_clock::now() + kTimeout;
    std::optional<std::string> first;
    while (!first && std::chrono::steady_clock::now() < deadline)
    {
        windows.push_back(create_window(conn, 10, 10, 200, 150));
        map_window(conn, windows.back());
        first = child.reader.read(child.output.fd, std::chrono::milliseconds(50));
    }
    REQUIRE(first);
    auto event = nlohmann::json::parse(*first);
    REQUIRE(event.at("event") == "window_map");
    REQUIRE(std::find(windows.begin(), windows.end(), event.at("window").get<xcb_window_t>()) != windows.end());

    child.output.reset();
    auto next = create_window(conn, 40, 40, 200, 150);
    map_window(conn, next);
    int status = 0;
    REQUIRE(child.wait(status));
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    rewind(child.errors.get());
    CHECK(fgetc(child.errors.get()) == EOF);
    destroy_window(conn, next);
    for (auto window : windows) destroy_window(conn, window);
}

TEST_CASE("Integration: subscription JSON preserves escaped focus fields", "[integration][subscribe][json]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto path = wait_for_ipc_socket_path(env->conn);
    REQUIRE(path);
    Subscriber subscriber(*path, "focus_change");
    auto window = create_window(env->conn, 10, 10, 200, 150);
    set_window_wm_class(env->conn, window, "escaped-instance", "escaped-class");
    std::string title = "quote \" and slash \\ line\n tab\t";
    set_window_title(env->conn, window, title);
    map_window(env->conn, window);
    auto event = subscriber.event();
    CHECK(event.at("event") == "focus_change");
    CHECK(event.at("window") == window);
    CHECK(event.at("class") == "escaped-class");
    CHECK(event.at("title") == title);
    destroy_window(env->conn, window);
}

TEST_CASE("Integration: subscription preserves monitor direction in key_action events", "[integration][subscribe]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    if (!extension_available(conn, &xcb_test_id))
        SKIP("XTEST extension not available");
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    Subscriber subscriber(*path, "key_action");
    REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("Left")));
    REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("Right")));
    for (auto action : { "focus_monitor_left", "focus_monitor_right" })
    {
        auto event = subscriber.event();
        CHECK(event.at("event") == "key_action");
        CHECK(event.at("action") == action);
    }
}

TEST_CASE("Integration: bindings and IPC report the same action events", "[integration][subscribe][actions]")
{
    auto env = TestEnvironment::create("[[binds]]\nkey = \"super+l\"\naction = 'ratio adjust 0.05'\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    if (!extension_available(conn, &xcb_test_id))
        SKIP("XTEST extension not available");
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    auto first = create_window(conn, 10, 10, 200, 200);
    auto second = create_window(conn, 10, 10, 200, 200);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, kTimeout));
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, kTimeout));
    Subscriber subscriber(*path, "key_action,layout_change");
    REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("l")));
    auto key = subscriber.event();
    CHECK(key.at("event") == "key_action");
    CHECK(key.at("action") == "adjust_ratio");
    auto by_key = subscriber.event();
    auto reply = send_ipc_command(*path, "ratio adjust 0.05");
    REQUIRE(reply);
    REQUIRE(reply->starts_with("ok"));
    auto by_ipc = subscriber.event();
    for (auto const& event : { by_key, by_ipc })
    {
        CHECK(event.at("event") == "layout_change");
        CHECK(event.at("action") == "adjust_ratio");
        CHECK(event.at("delta") == 0.05);
        CHECK_FALSE(event.contains("value"));
    }
    // Values keep their JSON type; absent values stay absent. Numeric events
    // must describe the accepted value, including precision beyond six decimals.
    for (auto const& [command, expected] : std::vector<std::pair<std::string, nlohmann::json>>{
             {     "layout set monocle",   { { "action", "set_layout" }, { "value", "monocle" } } },
             { "ratio set 0.6123456789", { { "action", "set_ratio" }, { "value", 0.6123456789 } } },
             {            "ratio reset",                         { { "action", "reset_ratios" } } }
    })
    {
        auto result = send_ipc_command(*path, command);
        REQUIRE(result);
        REQUIRE(result->starts_with("ok"));
        auto event = subscriber.event();
        REQUIRE(event.at("event") == "layout_change");
        REQUIRE(event.contains("instance"));
        REQUIRE(event.contains("sequence"));
        event.erase("event");
        event.erase("instance");
        event.erase("sequence");
        CHECK(event == expected);
    }
    destroy_window(conn, second);
    destroy_window(conn, first);
}
