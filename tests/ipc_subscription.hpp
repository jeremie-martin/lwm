#pragma once

#include "x11_test_harness.hpp"
#include <nlohmann/json.hpp>

namespace lwm::test {

// Preserve coalesced records and partial lines across bounded reads.
class LineReader
{
public:
    std::optional<std::string> read(int fd, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            auto end = pending_.find('\n');
            if (end != std::string::npos)
            {
                auto line = pending_.substr(0, end);
                pending_.erase(0, end + 1);
                return line;
            }
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                return { };
            pollfd descriptor{ fd, POLLIN, 0 };
            int ready = poll(&descriptor, 1, remaining.count());
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready <= 0)
                return { };
            char buffer[4096];
            auto count = ::read(fd, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return { };
            pending_.append(buffer, count);
        }
    }

private:
    std::string pending_;
};

struct TestFd
{
    int fd = -1;
    explicit TestFd(int value = -1)
        : fd(value)
    { }
    TestFd(TestFd const&) = delete;
    TestFd& operator=(TestFd const&) = delete;
    ~TestFd() { reset(); }
    void reset()
    {
        if (fd >= 0)
            close(std::exchange(fd, -1));
    }
};

// A real server acknowledgement establishes readiness, without timing assumptions.
struct Subscriber
{
    TestFd connection{ socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };
    int fd = connection.fd;
    LineReader reader;

    Subscriber(std::string const& path, std::string_view filter)
    {
        REQUIRE(fd >= 0);
        sockaddr_un address{ };
        address.sun_family = AF_UNIX;
        REQUIRE(path.size() < sizeof(address.sun_path));
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        std::string request = "subscribe " + std::string(filter) + "\n";
        REQUIRE(send(fd, request.data(), request.size(), MSG_NOSIGNAL) == request.size());
        REQUIRE(line() == "ok subscribed");
    }
    std::string line() { return reader.read(fd, std::chrono::seconds(2)).value_or(""); }
    nlohmann::json event()
    {
        auto text = line();
        REQUIRE_FALSE(text.empty());
        return nlohmann::json::parse(text);
    }
};
} // namespace lwm::test
