#include "x11_test_harness.hpp"
#include <X11/keysym.h>
#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

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

    auto ping = run_lwmctl(wm, { "ping" });
    REQUIRE(ping.has_value());
    REQUIRE(ping->exit_code == 0);
    REQUIRE(ping->stdout_text.find("pong") != std::string::npos);
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
    auto first = nlohmann::json::parse(command({ "log", "status" }));
    REQUIRE(first["active"] == true);
    command({ "exec", "/definitely/missing/lwm-binary" });
    REQUIRE(wait_for_condition(
        [&]
        {
            auto reply = run_lwmctl(wm, { "log", "status" });
            return reply && reply->exit_code == 0 && nlohmann::json::parse(reply->stdout_text)["active"] == true;
        },
        std::chrono::seconds(1)
    ));
    auto recovered = nlohmann::json::parse(command({ "log", "status" }));
    REQUIRE(recovered["instance"] == first["instance"]);
    REQUIRE(recovered["backend_notifications"] == 0);
    command({ "restart" });
    REQUIRE(wait_for_condition(
        [&]
        {
            auto reply = run_lwmctl(wm, { "log", "status" });
            return reply && reply->exit_code == 0
                && nlohmann::json::parse(reply->stdout_text)["instance"] != first["instance"];
        },
        std::chrono::seconds(1)
    ));
    REQUIRE(command({ "ping" }).find("pong") != std::string::npos);
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
    auto before = wm_owner(connection);
    for (int i = 0; i < 200; ++i) command({ "workspace", "switch", std::to_string(i % 2) });
    REQUIRE(command({ "ping" }).find("pong") != std::string::npos);
    auto logging_instance = nlohmann::json::parse(command({ "log", "status" }))["instance"];
    command({ "exec", "/definitely/missing/lwm-binary" });
    REQUIRE(wait_for_condition(
        [&]
        {
            auto owner = wm_owner(connection);
            return owner != XCB_NONE && owner != before && run_lwmctl(wm, { "ping" })->exit_code == 0;
        },
        std::chrono::seconds(2)
    ));
    REQUIRE(nlohmann::json::parse(command({ "log", "status" }))["instance"] == logging_instance);
    before = wm_owner(connection);
    command({ "restart" });
    REQUIRE(wait_for_condition(
        [&]
        {
            auto owner = wm_owner(connection);
            return owner != XCB_NONE && owner != before && run_lwmctl(wm, { "ping" })->exit_code == 0;
        },
        std::chrono::seconds(2)
    ));
    REQUIRE(command({ "ping" }).find("pong") != std::string::npos);
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
    wm.stop();
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
    auto ping = run_lwmctl(wm, { "ping" });
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
    auto ping = run_lwmctl(wm, { "ping" });
    REQUIRE(ping);
    REQUIRE(ping->exit_code == 0);
}

TEST_CASE("Integration: fullscreen diagnostics identify requests and resolved ownership", "[integration][logging]")
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
            return diagnostics.find("Fullscreen changed: window=" + id.str() + " enabled=true") != std::string::npos
                && diagnostics.find("Fullscreen owner changed: monitor=0 window=0x0 -> " + id.str())
                != std::string::npos;
        },
        std::chrono::seconds(2)
    ));
    send_client_message(connection, window, state, 0, fullscreen);
    REQUIRE(wait_for_condition(
        [&]
        {
            auto diagnostics = wm.diagnostics();
            return diagnostics.find("Fullscreen changed: window=" + id.str() + " enabled=false") != std::string::npos
                && diagnostics.find("Fullscreen owner changed: monitor=0 window=" + id.str() + " -> 0x0")
                != std::string::npos;
        },
        std::chrono::seconds(2)
    ));
}
