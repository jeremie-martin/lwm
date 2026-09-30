#include "x11_test_harness.hpp"
#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace lwm::test;

TEST_CASE("Integration: failed exec is reported at critical level and recovers", "[integration][logging][restart]")
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available; set LWM_TEST_ALLOW_EXISTING_DISPLAY=1 to use an existing DISPLAY.");

    X11Connection connection;
    REQUIRE(connection.ok());
    LwmProcess wm(environment.display(), {}, { "--log-level", "critical" });
    REQUIRE(wm.running());
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));

    auto restart = run_lwmctl(wm, { "exec", "/definitely/missing/lwm-binary" });
    REQUIRE(restart.has_value());
    REQUIRE(restart->exit_code == 0);
    REQUIRE(restart->stdout_text.find("restarting") != std::string::npos);

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

TEST_CASE(
    "Integration: saturated logging keeps IPC, failed exec, and restart responsive",
    "[integration][logging][restart]"
)
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available");
    X11Connection connection;
    REQUIRE(connection.ok());
    // Never drain the private journal socket: startup and workspace trace records fill it.
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
    REQUIRE(first["delivery_drops"].get<uint64_t>() > 0);
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
    REQUIRE(recovered["delivery_drops"].get<uint64_t>() >= first["delivery_drops"].get<uint64_t>());
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
    auto start = std::chrono::steady_clock::now();
    wm.stop();
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}
