#include "wm_observations.hpp"
#include <X11/keysym.h>
#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace lwm::test;

TEST_CASE("Integration: failed exec is reported at critical level and recovers", "[integration][logging][restart]")
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available; set LWM_TEST_ALLOW_EXISTING_DISPLAY=1 to use an existing DISPLAY.");

    X11Connection connection;
    REQUIRE(connection.ok());
    LwmProcess wm(environment.display(), { }, { "--log-level", "critical" });
    REQUIRE(wm.running());
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));

    auto restart = run_lwmctl(wm, { "exec", "/definitely/missing/lwm-binary" });
    REQUIRE(restart.has_value());
    REQUIRE(restart->exit_code == 0);
    REQUIRE(restart->stdout_text.empty());

    bool reported = wait_for_condition(
        [&wm]() { return wm.diagnostics().find("exec '/definitely/missing/lwm-binary' failed") != std::string::npos; },
        std::chrono::seconds(3)
    );
    REQUIRE(reported);

    auto ping = run_lwmctl(wm, { "version" });
    REQUIRE(ping.has_value());
    REQUIRE(ping->exit_code == 0);
    REQUIRE(!ping->stdout_text.empty());
}

TEST_CASE("Integration: standard logging survives failed exec and real restart", "[integration][logging][restart]")
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available");
    X11Connection connection;
    REQUIRE(connection.ok());
    LwmProcess wm(environment.display(), { }, { "--log-level", "trace" });
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));
    auto command = [&](std::vector<std::string> args)
    {
        auto start = std::chrono::steady_clock::now();
        auto reply = run_lwmctl(wm, args);
        REQUIRE(reply.has_value());
        REQUIRE(reply->exit_code == 0);
        REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
        return reply->stdout_text;
    };
    for (int i = 0; i < 30; ++i) command({ "workspace", "switch", std::to_string(i % 2) });
    // Every WM lifetime reports readiness through the surviving or restarted logger.
    auto ready = [&](size_t expected)
    {
        return wait_for_condition(
            [&]
            {
                auto log = wm.diagnostics();
                size_t count = 0;
                for (auto at = log.find("WM ready:"); at != std::string::npos; at = log.find("WM ready:", at + 1)) ++count;
                return count == expected;
            },
            std::chrono::seconds(2)
        );
    };
    REQUIRE(ready(1));
    command({ "exec", "/definitely/missing/lwm-binary" });
    REQUIRE(ready(2));
    command({ "restart" });
    REQUIRE(ready(3));
    REQUIRE(!command({ "version" }).empty());
    REQUIRE(wm.running());
    // ICCCM manager-selection takeover is LWM's orderly shutdown path.
    auto replacement = create_window(connection, 0, 0, 1, 1);
    auto selection = intern_atom(connection.get(), "WM_S0");
    auto owner_cookie = xcb_get_selection_owner(connection.get(), selection);
    auto* owner = xcb_get_selection_owner_reply(connection.get(), owner_cookie, nullptr);
    REQUIRE(owner);
    auto previous_owner = owner->owner;
    free(owner);
    REQUIRE(previous_owner != XCB_NONE);
    REQUIRE(previous_owner != replacement);
    xcb_set_selection_owner(connection.get(), replacement, selection, XCB_CURRENT_TIME);
    xcb_flush(connection.get());
    auto status = wm.wait_for_exit(std::chrono::seconds(1));
    REQUIRE(status);
    REQUIRE(WIFEXITED(*status));
    REQUIRE(WEXITSTATUS(*status) == 0);
    REQUIRE(wm.diagnostics().find("WM_S0 ownership transferred") != std::string::npos);
}

TEST_CASE("Integration: restart stays on the binary exec selected", "[integration][logging][restart]")
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available");
    X11Connection connection;
    REQUIRE(connection.ok());
    LwmProcess wm(environment.display(), { }, { "--log-level", "info" });
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));
    auto link = std::filesystem::temp_directory_path() / ("lwm-exec-" + std::to_string(getpid()));
    std::filesystem::remove(link);
    std::filesystem::create_symlink(LWM_BINARY_PATH, link);
    auto program = [&]
    {
        std::ifstream cmdline("/proc/" + std::to_string(wm.pid()) + "/cmdline");
        std::string argv0;
        std::getline(cmdline, argv0, '\0');
        return argv0;
    };
    auto restart = [&](std::vector<std::string> args)
    {
        auto previous = wm_instance(connection);
        REQUIRE(previous);
        REQUIRE(run_lwmctl(wm, args));
        REQUIRE(wait_for_wm_restart(connection, std::chrono::seconds(3), *previous));
    };
    restart({ "exec", link.string() });
    CHECK(program() == link.string());
    restart({ "restart" });
    CHECK(program() == link.string());
    // At the default level the backend drains slowly; the predecessor's last
    // lines are still written before exec replaces it.
    CHECK(wm.diagnostics().find("Restart state serialized") != std::string::npos);
    std::filesystem::remove(link);
}

TEST_CASE("Integration: losing the screen wins over a restart queued in either order", "[integration][logging][restart]")
{
    bool restart_first = GENERATE(false, true);
    CAPTURE(restart_first);
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available");
    X11Connection connection;
    REQUIRE(connection.ok());
    LwmProcess wm(environment.display(), { }, { "--log-level", "info" });
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));
    auto* c = connection.get();
    auto selection = intern_atom(c, "WM_S0");
    auto command = intern_atom(c, "_LWM_COMMAND");
    auto owner = xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, selection), nullptr);
    REQUIRE(owner);
    auto wm_window = owner->owner;
    free(owner);
    auto requester = create_window(connection, 0, 0, 1, 1);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, requester, command, intern_atom(c, "UTF8_STRING"), 8, 7, "restart");
    auto replacement = create_window(connection, 0, 0, 1, 1);
    // Both reach LWM in one read: the takeover and a restart request.
    auto restart = [&]
    { lwm::send_message(c, wm_window, command, XCB_EVENT_MASK_NO_EVENT, { .data32 = { requester } }); };
    if (restart_first)
        restart();
    xcb_set_selection_owner(c, replacement, selection, XCB_CURRENT_TIME);
    if (!restart_first)
        restart();
    xcb_flush(c);
    auto status = wm.wait_for_exit(std::chrono::seconds(2));
    REQUIRE(status);
    REQUIRE(WIFEXITED(*status));
    CHECK(WEXITSTATUS(*status) == 0);
    CHECK(wm.diagnostics().find("Restarting") == std::string::npos);
    CHECK(wm_owner(connection) == replacement);
}

TEST_CASE("Integration: a restart variable inherited from another process is ignored", "[integration][logging][restart]")
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available");
    X11Connection connection;
    REQUIRE(connection.ok());
    // Another process's handoff, or one naming only a descriptor, which here may be
    // this WM's own X connection.
    auto value = GENERATE("1 4", "4");
    CAPTURE(value);
    LwmProcess wm(environment.display(), { }, { "--log-level", "info" }, -1, find_test_executable_path("lwm"),
                  { { "LWM_RESTART_FD", value } });
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));
    REQUIRE(wait_for_condition([&] { return wm.diagnostics().find("WM ready") != std::string::npos; }, std::chrono::seconds(2)));
    CHECK(wm.diagnostics().find("restart=false") != std::string::npos);
    auto version = run_lwmctl(wm, { "version" });
    REQUIRE(version);
    CHECK(version->exit_code == 0);
}

TEST_CASE(
    "Integration: blocked console leaves the event loop and exec restart responsive",
    "[integration][logging][restart]"
)
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available");
    X11Connection connection;
    REQUIRE(connection.ok());
    struct Pipe
    {
        int fds[2]{ -1, -1 };
        ~Pipe()
        {
            for (int fd : fds)
                if (fd >= 0)
                    close(fd);
        }
    } pipe;
    REQUIRE(pipe2(pipe.fds, O_CLOEXEC | O_NONBLOCK) == 0);
    std::string fill(4096, 'x');
    while (write(pipe.fds[1], fill.data(), fill.size()) > 0)
    { }
    REQUIRE(errno == EAGAIN);
    REQUIRE(fcntl(pipe.fds[1], F_SETFL, fcntl(pipe.fds[1], F_GETFL) & ~O_NONBLOCK) == 0);
    LwmProcess wm(environment.display(), { }, { "--log-level", "trace" }, pipe.fds[1]);
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));
    auto command = [&](std::vector<std::string> args)
    {
        auto started = std::chrono::steady_clock::now();
        auto result = run_lwmctl(wm, args);
        REQUIRE(result.has_value());
        REQUIRE(result->exit_code == 0);
        REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
        return result->stdout_text;
    };
    auto threads = [&]
    {
        auto tasks = std::filesystem::directory_iterator("/proc/" + std::to_string(wm.pid()) + "/task");
        return std::distance(tasks, std::filesystem::directory_iterator{ });
    };
    REQUIRE(threads() == 2);
    for (int i = 0; i < 200; ++i) command({ "workspace", "switch", std::to_string(i % 2) });
    REQUIRE(!command({ "version" }).empty());
    auto restart = [&](std::vector<std::string> args)
    {
        auto before = wm_owner(connection);
        command(std::move(args));
        REQUIRE(wait_for_condition(
            [&]
            {
                auto owner = wm_owner(connection);
                if (owner == XCB_NONE || owner == before)
                    return false;
                auto reply = run_lwmctl(wm, { "version" });
                return reply && reply->exit_code == 0;
            },
            std::chrono::seconds(2)
        ));
    };
    for (int i = 0; i < 20; ++i)
    {
        CAPTURE(i);
        restart({ "exec", "/definitely/missing/lwm-binary" });
        REQUIRE(threads() == 3); // Main, backend, and one outstanding flush.
    }
    restart({ "restart" });
    REQUIRE(threads() == 2);
    restart({ "exec", "/definitely/missing/lwm-binary" });
    REQUIRE(threads() == 3);
    REQUIRE(!command({ "version" }).empty());
    auto shutdown = [&]
    {
        auto replacement = create_window(connection, 0, 0, 1, 1);
        xcb_set_selection_owner(connection.get(), replacement, intern_atom(connection.get(), "WM_S0"), XCB_CURRENT_TIME);
        xcb_flush(connection.get());
    };
    bool shutting_down = false;
    SECTION("delivery resumes before shutdown") { }
    SECTION("shutdown owns the outstanding flush")
    {
        shutdown();
        shutting_down = true;
        // The destination remains blocked, so orderly shutdown must still wait.
        REQUIRE_FALSE(wm.wait_for_exit(std::chrono::milliseconds(100)));
    }
    // Only normal shutdown drains the logger. Resume the reader for that step.
    std::jthread reader(
        [&](std::stop_token stop)
        {
            char buffer[8192];
            while (!stop.stop_requested())
            {
                while (read(pipe.fds[0], buffer, sizeof(buffer)) > 0)
                { }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    );
    if (!shutting_down)
    {
        REQUIRE(wait_for_condition([&] { return threads() == 2; }, std::chrono::seconds(2)));
        shutdown();
    }
    auto status = wm.wait_for_exit(std::chrono::seconds(2));
    REQUIRE(status);
    REQUIRE(WIFEXITED(*status));
    REQUIRE(WEXITSTATUS(*status) == 0);
}

TEST_CASE("Integration: launch failures identify the command without disclosing arguments", "[integration][logging]")
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available");
    X11Connection connection;
    REQUIRE(connection.ok());
    LwmProcess wm(
        environment.display(),
        R"(
[binds]
F7 = ["/definitely/missing/lwm-application", "private-argument"]
)",
        { "--log-level", "debug" }
    );
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));
    if (!extension_available(connection, &xcb_test_id))
        SKIP("XTEST extension not available");
    REQUIRE(send_key(connection, XK_F7));
    REQUIRE(wait_for_condition(
        [&] { return wm.diagnostics().find("Launch failed:") != std::string::npos; },
        std::chrono::seconds(2)
    ));
    auto diagnostics = wm.diagnostics();
    REQUIRE(
        diagnostics.find("Launch failed: source=keybind executable=/definitely/missing/lwm-application")
        != std::string::npos
    );
    REQUIRE(diagnostics.find("stage=spawn code=" + std::to_string(ENOENT)) != std::string::npos);
    REQUIRE(diagnostics.find("private-argument") == std::string::npos);
    auto ping = run_lwmctl(wm, { "version" });
    REQUIRE(ping);
    REQUIRE(ping->exit_code == 0);
}

TEST_CASE("Integration: asynchronous X failures retain protocol context", "[integration][logging]")
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available");
    X11Connection connection;
    REQUIRE(connection.ok());
    bool resource_race = false;
    SECTION("invalid raw restack is a warning") { }
    SECTION("destroyed resource is debug context") { resource_race = true; }
    LwmProcess wm(environment.display(), { }, { "--log-level", resource_race ? "debug" : "warn" });
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));
    auto window = create_window(connection, 0, 0, 100, 100);
    if (resource_race)
    {
        xcb_destroy_window(connection.get(), window);
        // The WM responds with ChangeProperty on this now-missing resource.
        send_client_message(connection, window, intern_atom(connection.get(), "_NET_REQUEST_FRAME_EXTENTS"), 0);
    }
    else
    {
        // An unmapped, unmanaged window takes the raw ConfigureWindow path.
        send_client_message(connection, window, intern_atom(connection.get(), "_NET_RESTACK_WINDOW"), 0, 0, 999);
    }
    auto expected = resource_race ? "X resource error: code=3 opcode=18:0" : "X protocol error: code=2 opcode=12:0";
    REQUIRE(wait_for_condition(
        [&] { return wm.diagnostics().find(expected) != std::string::npos; },
        std::chrono::seconds(2)
    ));
    auto diagnostics = wm.diagnostics();
    REQUIRE(diagnostics.find("resource=0x") != std::string::npos);
    REQUIRE(diagnostics.find("sequence=") != std::string::npos);
    auto ping = run_lwmctl(wm, { "version" });
    REQUIRE(ping);
    REQUIRE(ping->exit_code == 0);
}

TEST_CASE("Integration: fullscreen diagnostics identify requests", "[integration][logging]")
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available");
    X11Connection connection;
    REQUIRE(connection.ok());
    LwmProcess wm(environment.display(), { }, { "--log-level", "debug" });
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));
    auto window = create_window(connection, 20, 20, 320, 240);
    map_window(connection, window);
    REQUIRE(wait_for_active_window(connection, window, std::chrono::seconds(2)));
    auto state = intern_atom(connection.get(), "_NET_WM_STATE");
    auto fullscreen = intern_atom(connection.get(), "_NET_WM_STATE_FULLSCREEN");
    std::ostringstream id;
    id << "0x" << std::hex << window;
    send_client_message(connection, window, state, 1, fullscreen);
    REQUIRE(wait_for_condition(
        [&]
        {
            auto diagnostics = wm.diagnostics();
            return diagnostics.find("Fullscreen changed: window=" + id.str() + " enabled=true") != std::string::npos;
        },
        std::chrono::seconds(2)
    ));
    send_client_message(connection, window, state, 0, fullscreen);
    REQUIRE(wait_for_condition(
        [&]
        {
            auto diagnostics = wm.diagnostics();
            return diagnostics.find("Fullscreen changed: window=" + id.str() + " enabled=false") != std::string::npos;
        },
        std::chrono::seconds(2)
    ));
}
