#include "wm_observations.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <lwm/wm.hpp>

using namespace lwm;
using namespace lwm::test;

TEST_CASE("Failed WM construction releases its descriptors", "[integration][lifecycle]")
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto descriptors = []
    {
        return std::distance(
            std::filesystem::directory_iterator("/proc/self/fd"),
            std::filesystem::directory_iterator{ }
        );
    };
    auto before = descriptors();
    for (int i = 0; i < 3; ++i)
    {
        // A real existing WM forces failure after the new X connection is opened.
        REQUIRE_THROWS_WITH(
            WindowManager(default_config(), ""), Catch::Matchers::ContainsSubstring("Another window manager")
        );
        CHECK(descriptors() == before);
    }
}

TEST_CASE("Unexpected event-loop exceptions terminate instead of reconstructing", "[integration][lifecycle]")
{
    auto& server = X11TestEnvironment::instance();
    if (!server.available())
        SKIP("X11 unavailable");
    X11Connection conn;
    REQUIRE(conn.ok());
    auto result = run_command(
        LWM_RUNTIME_FAILURE_PROBE_PATH,
        {
            "--log-target",
            "stderr",
            "--log-level",
            "critical"
    },
        { { "DISPLAY", server.display() } },
        { "XDG_CONFIG_HOME" }
    );
    REQUIRE(result);
    INFO(result->stderr_text);
    REQUIRE(result->exit_code == 1);
    REQUIRE(result->stderr_text.find("injected event-loop failure") != std::string::npos);
    // Failure released WM ownership; a fresh process can claim it normally.
    LwmProcess replacement(server.display());
    REQUIRE(wait_for_wm_ready(conn, std::chrono::seconds(2)));
}

TEST_CASE("Startup failure exits without disturbing the running WM", "[integration][lifecycle]")
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto instance = wm_instance(env->conn);
    REQUIRE(instance);
    LwmProcess duplicate(X11TestEnvironment::instance().display());
    auto status = duplicate.wait_for_exit(std::chrono::seconds(2));
    REQUIRE(status);
    REQUIRE(WIFEXITED(*status));
    REQUIRE(WEXITSTATUS(*status) == 1);
    REQUIRE(duplicate.diagnostics().find("Another window manager") != std::string::npos);
    REQUIRE(wm_instance(env->conn) == instance);
}
