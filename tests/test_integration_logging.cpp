#include "x11_test_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace lwm::test;

namespace {

std::string read_file(std::filesystem::path const& path)
{
    std::ifstream input(path);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

} // namespace

TEST_CASE("Integration: failed exec is reported at critical level and recovers", "[integration][logging][restart]")
{
    auto& environment = X11TestEnvironment::instance();
    if (!environment.available())
        SKIP("Xvfb not available; set LWM_TEST_ALLOW_EXISTING_DISPLAY=1 to use an existing DISPLAY.");
    if (lwmctl_executable_path().empty())
        SKIP("lwmctl binary not available");

    X11Connection connection;
    REQUIRE(connection.ok());
    LwmProcess wm(environment.display(), {}, { "--log-level", "critical" });
    REQUIRE(wm.running());
    REQUIRE(wait_for_wm_ready(connection, std::chrono::seconds(2)));

    auto restart = run_lwmctl(wm, { "exec", "/definitely/missing/lwm-binary" });
    REQUIRE(restart.has_value());
    REQUIRE(restart->exit_code == 0);
    REQUIRE(restart->stdout_text.find("restarting") != std::string::npos);

    auto log_path = std::filesystem::path(wm.runtime_dir()) / "lwm"
        / ("lwm-" + std::to_string(static_cast<unsigned long long>(wm.pid())) + ".log");
    bool reported = wait_for_condition(
        [&log_path]()
        {
            return std::filesystem::exists(log_path)
                && read_file(log_path).find("exec '/definitely/missing/lwm-binary' failed") != std::string::npos;
        },
        std::chrono::seconds(3)
    );
    REQUIRE(reported);

    auto ping = run_lwmctl(wm, { "ping" });
    REQUIRE(ping.has_value());
    REQUIRE(ping->exit_code == 0);
    REQUIRE(ping->stdout_text.find("pong") != std::string::npos);
}
