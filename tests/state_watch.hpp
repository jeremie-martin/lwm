#pragma once

#include "test_resources.hpp"
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

// The real `lwmctl watch` on the test display. Its first line is the state at
// attachment; each later line is a changed state.
struct Watcher
{
    TestFd output;
    LineReader reader;
    pid_t pid = -1;

    Watcher()
    {
        int pipe[2];
        REQUIRE(pipe2(pipe, O_CLOEXEC) == 0);
        output.fd = pipe[0];
        TestFd writer{ pipe[1] };
        auto executable = lwmctl_executable_path();
        pid = fork();
        REQUIRE(pid >= 0);
        if (pid == 0)
        {
            if (dup2(writer.fd, STDOUT_FILENO) < 0)
                _exit(126);
            execl(executable.c_str(), executable.c_str(), "watch", nullptr);
            _exit(127);
        }
    }
    Watcher(Watcher const&) = delete;
    Watcher& operator=(Watcher const&) = delete;
    ~Watcher()
    {
        if (pid > 0)
        {
            kill(pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
            { }
        }
    }

    std::optional<std::string> line(std::chrono::milliseconds timeout = std::chrono::seconds(2))
    {
        return reader.read(output.fd, timeout);
    }
    nlohmann::json state()
    {
        auto text = line();
        REQUIRE(text);
        return nlohmann::json::parse(*text);
    }
    // No further state is printed within a short settling period.
    bool quiet() { return !line(std::chrono::milliseconds(50)); }
};

} // namespace lwm::test
