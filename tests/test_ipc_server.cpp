#include "lwm/core/ipc_server.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using lwm::ipc::Server;

namespace {
struct Peer
{
    int fd = -1;
    ~Peer()
    {
        if (fd >= 0)
            close(fd);
    }
    explicit Peer(std::string const& path)
    {
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        REQUIRE(fd >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strcpy(address.sun_path, path.c_str());
        REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    }
    void send(std::string_view text)
    {
        REQUIRE(::send(fd, text.data(), text.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(text.size()));
    }
};

struct Fixture
{
    std::filesystem::path directory;
    Server server;
    std::string reply = "ok pong";
    std::vector<std::string> requests;
    Fixture()
    {
        char pattern[] = "/tmp/lwm-ipc-test-XXXXXX";
        auto path = mkdtemp(pattern);
        REQUIRE(path);
        directory = path;
        server.start((directory / "ipc.sock").string());
    }
    ~Fixture()
    {
        server.stop();
        std::filesystem::remove_all(directory);
    }
    void pump()
    {
        std::vector<pollfd> fds;
        server.append_poll_fds(fds);
        poll(fds.data(), fds.size(), 1);
        server.dispatch(
            fds,
            [&](std::string const& request)
            {
                requests.push_back(request);
                return reply;
            }
        );
    }
    std::string receive(Peer& peer, bool until_eof = true)
    {
        std::string text;
        auto deadline = Server::Clock::now() + std::chrono::seconds(2);
        while (Server::Clock::now() < deadline)
        {
            pump();
            char buffer[16384];
            auto n = recv(peer.fd, buffer, sizeof(buffer), 0);
            if (n == 0)
                return text;
            if (n > 0)
            {
                text.append(buffer, n);
                if (!until_eof && text.ends_with('\n'))
                    return text;
            }
            else
                REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK));
        }
        FAIL("IPC response did not complete");
        return {};
    }
};
}

TEST_CASE("IPC assembles partial requests and preserves EOF framing", "[ipc][transport]")
{
    Fixture f;
    Peer peer(f.server.path());
    peer.send("pi");
    f.pump();
    f.pump();
    CHECK(f.requests.empty());
    SECTION("newline terminates the first command") { peer.send("ng\nignored\n"); }
    SECTION("half-close terminates the command")
    {
        peer.send("ng");
        REQUIRE(shutdown(peer.fd, SHUT_WR) == 0);
    }
    CHECK(f.receive(peer) == "ok pong\n");
    CHECK(f.requests == std::vector<std::string>{ "ping" });
}

TEST_CASE("IPC finishes large replies across partial writes", "[ipc][transport]")
{
    Fixture f;
    f.reply = "ok " + std::string(2 * 1024 * 1024, 'x');
    Peer peer(f.server.path());
    peer.send("window list\n");
    // Fill the send buffer before reading. The response cannot fit in one send.
    for (int i = 0; i < 12; ++i) f.pump();
    REQUIRE(f.requests.size() == 1);
    CHECK(f.receive(peer) == f.reply + '\n');
}

TEST_CASE("IPC bounds requests and rejects concurrent incomplete requests", "[ipc][transport]")
{
    Fixture f;
    Peer first(f.server.path());
    first.send("partial");
    f.pump();
    f.pump();
    Peer second(f.server.path());
    CHECK(f.receive(second) == "error busy\n");
    first.send(std::string(4096, 'x'));
    CHECK(f.receive(first) == "error request too large\n");
    CHECK(f.requests.empty());
}

TEST_CASE("IPC releases stalled readers and writers at the deadline", "[ipc][transport]")
{
    Fixture f;
    Peer stalled(f.server.path());
    SECTION("incomplete request") { stalled.send("pi"); }
    SECTION("unread large reply")
    {
        f.reply = "ok " + std::string(2 * 1024 * 1024, 'x');
        stalled.send("window list\n");
    }
    f.pump();
    f.pump();
    REQUIRE(f.server.deadline());
    auto limit = Server::Clock::now() + std::chrono::seconds(2);
    while (f.server.deadline() && Server::Clock::now() < limit) f.pump();
    REQUIRE_FALSE(f.server.deadline());
    f.reply = "ok pong";
    Peer next(f.server.path());
    next.send("ping\n");
    CHECK(f.receive(next) == "ok pong\n");
}

TEST_CASE("IPC subscription acknowledgement precedes filtered events", "[ipc][transport]")
{
    Fixture f;
    Peer peer(f.server.path());
    peer.send("subscribe focus_change\n");
    CHECK(f.receive(peer, false) == "ok subscribed\n");
    CHECK(f.requests.empty());
    f.server.emit(lwm::Event_WindowMap, "ignored");
    f.server.emit(lwm::Event_FocusChange, "{\"event\":\"focus_change\"}");
    CHECK(f.receive(peer, false) == "{\"event\":\"focus_change\"}\n");
}
