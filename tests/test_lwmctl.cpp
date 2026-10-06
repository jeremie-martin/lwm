#include "test_resources.hpp"
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

    ReplyServer(
        std::string const& expected,
        std::string const& reply,
        int before_ms = 0,
        int after_line_ms = 0,
        int backlog_delay_ms = 0,
        int byte_delay_ms = 0,
        size_t paced_from = 0
    )
    {
        char pattern[] = "/tmp/lwmctl-test-XXXXXX";
        auto* created = mkdtemp(pattern);
        REQUIRE(created);
        directory = created;
        path = directory + "/ipc.sock";
        TestFd listener_owner{ socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };
        int listener = listener_owner.fd;
        REQUIRE(listener >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strcpy(address.sun_path, path.c_str());
        REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(listen(listener, 1) == 0);
        std::vector<TestFd> queued;
        if (backlog_delay_ms)
        {
            // Linux permits backlog + 1 queued connections. Verify saturation before launching the CLI.
            for (int i = 0; i < 3; ++i)
            {
                TestFd owner{ socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) };
                int fd = owner.fd;
                REQUIRE(fd >= 0);
                int result = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
                if (i < 2)
                {
                    REQUIRE(result == 0);
                    queued.push_back(std::move(owner));
                }
                else
                {
                    CHECK(result == -1);
                    CHECK(errno == EAGAIN);
                }
            }
        }
        child = fork();
        if (child == 0)
        {
            alarm(5);
            if (backlog_delay_ms)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(backlog_delay_ms));
                for (auto& queued_fd : queued)
                {
                    queued_fd.reset();
                    int accepted = accept(listener, nullptr, nullptr);
                    if (accepted < 0)
                        _exit(5);
                    close(accepted);
                }
            }
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
            for (size_t i = 0; i < reply.size(); ++i)
            {
                char byte = reply[i];
                if (byte_delay_ms && i >= paced_from)
                    std::this_thread::sleep_for(std::chrono::milliseconds(byte_delay_ms));
                if (send(fd, &byte, 1, MSG_NOSIGNAL) != 1)
                    _exit(byte_delay_ms && errno == EPIPE ? 0 : 4);
                if (byte == '\n' && after_line_ms)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(after_line_ms));
                    after_line_ms = 0;
                }
            }
            close(fd);
            _exit(0);
        }
        queued.clear();
        listener_owner.reset();
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

TEST_CASE("lwmctl bounds a silent peer", "[ipc][lwmctl]")
{
    ReplyServer server("ping\n", "", 200);
    auto result = run_command(lwmctl_executable_path(), { "--socket", server.path, "--timeout", "50", "ping" });
    REQUIRE(result);
    CHECK(result->exit_code == 1);
    CHECK(result->stderr_text.find("timed out") != std::string::npos);
    server.finish();
}

TEST_CASE("lwmctl deadlines bound complete lines despite continuing byte delivery", "[ipc][lwmctl]")
{
    std::string request = "ping", reply = "ok pong\n";
    ReplyServer server(request + "\n", reply, 0, 0, 0, 50);
    auto started = std::chrono::steady_clock::now();
    auto result = run_command(lwmctl_executable_path(), { "--socket", server.path, "--timeout", "150", request });
    REQUIRE(result);
    CHECK(result->exit_code == 1);
    CHECK(result->stderr_text.find("timed out") != std::string::npos);
    CHECK(result->stdout_text.empty());
    // Leave scheduling headroom while rejecting a timeout renewed for every byte.
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    server.finish();
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

TEST_CASE("lwmctl waits for Unix listener capacity within its connection deadline", "[ipc][lwmctl]")
{
    SECTION("capacity becomes available")
    {
        ReplyServer server("ping\n", "ok pong\n", 0, 0, 200);
        auto result = run_command(lwmctl_executable_path(), { "--socket", server.path, "--timeout", "1000", "ping" });
        REQUIRE(result);
        CHECK(result->exit_code == 0);
        CHECK(result->stdout_text == "pong\n");
        CHECK(result->stderr_text.empty());
        server.finish();
    }
    SECTION("capacity remains unavailable through the deadline")
    {
        ReplyServer server("ping\n", "ok pong\n", 0, 0, 2000);
        auto start = std::chrono::steady_clock::now();
        auto result = run_command(lwmctl_executable_path(), { "--socket", server.path, "--timeout", "100", "ping" });
        auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(result);
        CHECK(result->exit_code == 1);
        CHECK(result->stdout_text.empty());
        CHECK(result->stderr_text.find("timed out") != std::string::npos);
        CHECK(elapsed >= std::chrono::milliseconds(100));
        CHECK(elapsed < std::chrono::seconds(1));
    }
}

TEST_CASE("lwmctl rejects notification metadata before connecting", "[ipc][lwmctl]")
{
    for (auto const& arg : { "app-name=Ghostty", "desktop-entry=Ghostty", "window=123 app-name=Ghostty" })
    {
        auto result =
            run_command(LWMCTL_BINARY_PATH, { "--socket", "/nonexistent/lwm-test.sock", "notify-attention", arg });
        REQUIRE(result);
        REQUIRE(result->exit_code == 1);
        REQUIRE(result->stderr_text.find("usage: notify-attention window=<xid>") != std::string::npos);
    }
}
