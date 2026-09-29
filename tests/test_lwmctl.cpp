#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>

using namespace lwm::test;

namespace {
// A bounded, independent wire peer exercises the real CLI without a WM or X server.
struct ReplyServer
{
    std::string directory;
    std::string path;
    pid_t child = -1;

    ReplyServer(std::string const& expected, std::string const& reply, int before_ms = 0, int after_line_ms = 0)
    {
        char pattern[] = "/tmp/lwmctl-test-XXXXXX";
        auto* created = mkdtemp(pattern);
        REQUIRE(created);
        directory = created;
        path = directory + "/ipc.sock";
        int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        REQUIRE(listener >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strcpy(address.sun_path, path.c_str());
        REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(listen(listener, 1) == 0);
        child = fork();
        if (child == 0)
        {
            alarm(5);
            int fd = accept(listener, nullptr, nullptr);
            if (fd < 0)
                _exit(2);
            std::string request;
            char ch;
            while (recv(fd, &ch, 1, 0) == 1 && request.size() < 4096)
            {
                request.push_back(ch);
                if (ch == '\n')
                    break;
            }
            if (request != expected)
                _exit(3);
            std::this_thread::sleep_for(std::chrono::milliseconds(before_ms));
            // Single-byte writes exercise framing independently of packet boundaries.
            for (char byte : reply)
            {
                if (send(fd, &byte, 1, MSG_NOSIGNAL) != 1)
                    _exit(4);
                if (byte == '\n' && after_line_ms)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(after_line_ms));
                    after_line_ms = 0;
                }
            }
            close(fd);
            _exit(0);
        }
        close(listener);
        REQUIRE(child > 0);
    }
    ~ReplyServer()
    {
        if (child > 0)
        {
            kill(child, SIGKILL);
            waitpid(child, nullptr, 0);
        }
        std::filesystem::remove_all(directory);
    }
    void finish()
    {
        int status = 0;
        REQUIRE(waitpid(child, &status, 0) == child);
        child = -1;
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
    }
};
}

TEST_CASE("lwmctl requires a complete recognized command reply", "[ipc][lwmctl]")
{
    std::string reply;
    int expected_exit = 1;
    std::string output;
    SECTION("empty") { }
    SECTION("truncated success") { reply = "ok pong"; }
    SECTION("unknown envelope") { reply = "pong\n"; }
    SECTION("multiple replies") { reply = "ok\nok\n"; }
    SECTION("server error") { reply = "error busy\n"; }
    SECTION("success without value")
    {
        reply = "ok\n";
        expected_exit = 0;
    }
    SECTION("success with value")
    {
        reply = "ok pong\n";
        expected_exit = 0;
        output = "pong\n";
    }
    ReplyServer server("ping\n", reply);
    auto result = run_command(lwmctl_executable_path(), { "--socket", server.path, "ping" });
    REQUIRE(result);
    CHECK(result->exit_code == expected_exit);
    CHECK(result->stdout_text == output);
    if (expected_exit)
        CHECK_FALSE(result->stderr_text.empty());
    server.finish();
}

TEST_CASE("lwmctl validates subscription acknowledgement before streaming", "[ipc][lwmctl]")
{
    std::string reply;
    int expected_exit = 1;
    std::string output;
    SECTION("empty") { }
    SECTION("truncated acknowledgement") { reply = "ok subscribed"; }
    SECTION("unknown acknowledgement") { reply = "anything\n"; }
    SECTION("ordinary success is not subscription confirmation") { reply = "ok\n"; }
    SECTION("server error") { reply = "error busy\n"; }
    SECTION("truncated event") { reply = "ok subscribed\n{\"event\":\"focus_change\""; }
    SECTION("acknowledgement and event")
    {
        reply = "ok subscribed\n{\"event\":\"focus_change\"}\n";
        expected_exit = 0;
        output = "{\"event\":\"focus_change\"}\n";
    }
    ReplyServer server("subscribe focus_change\n", reply);
    auto result = run_command(lwmctl_executable_path(), { "--socket", server.path, "subscribe", "focus_change" });
    REQUIRE(result);
    CHECK(result->exit_code == expected_exit);
    CHECK(result->stdout_text == output);
    if (expected_exit)
        CHECK_FALSE(result->stderr_text.empty());
    server.finish();
}

TEST_CASE("lwmctl bounds handshakes but permits idle subscriptions", "[ipc][lwmctl]")
{
    SECTION("silent peer times out")
    {
        ReplyServer server("ping\n", "", 200);
        auto result = run_command(lwmctl_executable_path(), { "--socket", server.path, "--timeout", "50", "ping" });
        REQUIRE(result);
        CHECK(result->exit_code == 1);
        CHECK(result->stderr_text.find("timed out") != std::string::npos);
        server.finish();
    }
    SECTION("idle stream does not inherit the handshake deadline")
    {
        ReplyServer server("subscribe\n", "ok subscribed\n{\"event\":\"future\"}\n", 0, 200);
        auto result =
            run_command(lwmctl_executable_path(), { "--socket", server.path, "--timeout", "50", "subscribe" });
        REQUIRE(result);
        CHECK(result->exit_code == 0);
        CHECK(result->stdout_text == "{\"event\":\"future\"}\n");
        server.finish();
    }
}

TEST_CASE("lwmctl offers local help and preserves option-like names after double dash", "[ipc][lwmctl]")
{
    auto help = run_command(lwmctl_executable_path(), { "workspace", "--help" });
    REQUIRE(help);
    CHECK(help->exit_code == 0);
    CHECK(help->stderr_text.empty());
    CHECK(help->stdout_text.find("workspace switch N") != std::string::npos);
    CHECK(help->stdout_text.find("scratchpad stash") == std::string::npos);
    ReplyServer server("scratchpad toggle --help\n", "ok\n");
    auto result =
        run_command(lwmctl_executable_path(), { "--socket", server.path, "--", "scratchpad", "toggle", "--help" });
    REQUIRE(result);
    CHECK(result->exit_code == 0);
    server.finish();
}
