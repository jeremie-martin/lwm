#include "wm_observations.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <lwm/core/signals.hpp>
#include <lwm/wm.hpp>
#include <poll.h>

using namespace lwm;
using namespace lwm::test;

TEST_CASE("Signal ownership survives failed WM construction and releases descriptors", "[integration][lifecycle]")
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
    struct sigaction previous{ };
    REQUIRE(sigaction(SIGHUP, nullptr, &previous) == 0);
    {
        SignalPipe signals;
        auto owned = descriptors();
        for (int i = 0; i < 3; ++i)
        {
            // A real existing WM forces failure after the new X connection is opened.
            REQUIRE_THROWS_WITH(
                WindowManager(default_config(), signals, ""),
                Catch::Matchers::ContainsSubstring("Another window manager")
            );
            CHECK(descriptors() == owned);
            REQUIRE(raise(SIGHUP) == 0);
            pollfd fd{ signals.fd(), POLLIN, 0 };
            REQUIRE(poll(&fd, 1, 1000) == 1);
            CHECK((fd.revents & POLLIN) != 0);
            signals.drain();
            CHECK(poll(&fd, 1, 0) == 0);
        }
    }
    CHECK(descriptors() == before);
    struct sigaction restored{ };
    REQUIRE(sigaction(SIGHUP, nullptr, &restored) == 0);
    CHECK(restored.sa_handler == previous.sa_handler);
}

TEST_CASE("SIGHUP reload survives WM reconstruction after failed exec", "[integration][lifecycle][restart]")
{
    auto env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    REQUIRE(env);
    auto names = intern_atom(env->conn.get(), "_NET_DESKTOP_NAMES");
    for (std::string expected : { "first", "second" })
    {
        auto previous = wm_instance(env->conn);
        REQUIRE(previous);
        auto result = run_lwmctl(env->wm, { "exec", "/definitely/missing/lwm-binary" });
        REQUIRE(result);
        REQUIRE(result->exit_code == 0);
        REQUIRE(wait_for_wm_restart(env->conn, std::chrono::seconds(2), *previous));
        REQUIRE(env->wm.write_config("[workspaces]\ncount = 2\nnames = [\"" + expected + "\", \"other\"]\n"));
        REQUIRE(kill(env->wm.pid(), SIGHUP) == 0);
        REQUIRE(wait_for_property_strings(
            env->conn.get(),
            env->conn.root(),
            names,
            { expected, "other" },
            std::chrono::seconds(2)
        ));
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
    LwmProcess duplicate(X11TestEnvironment::instance().display());
    auto status = duplicate.wait_for_exit(std::chrono::seconds(2));
    REQUIRE(status);
    REQUIRE(WIFEXITED(*status));
    REQUIRE(WEXITSTATUS(*status) == 1);
    REQUIRE(duplicate.diagnostics().find("Another window manager") != std::string::npos);
    REQUIRE(wm_instance(env->conn) == instance);
}
