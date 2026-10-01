#include "lwm/core/log.hpp"
#ifndef NDEBUG
#    include "lwm/core/invariants.hpp"
#    include "state_fixture.hpp"
#    include <sys/prctl.h>
#endif
#include <array>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// Test-only address redirection: exercise the real SystemdSink, libsystemd
// encoder and kernel backpressure without touching the host journal. No return
// values or I/O behavior are simulated. Only this probe exports the interposer.
extern "C" ssize_t sendmsg(int fd, msghdr const* message, int flags)
{
    auto copy = *message;
    sockaddr_un address{ };
    if (message->msg_name && message->msg_namelen >= sizeof(sa_family_t))
    {
        auto* original = static_cast<sockaddr_un const*>(message->msg_name);
        if (original->sun_family == AF_UNIX && std::string_view(original->sun_path) == "/run/systemd/journal/socket")
        {
            char const* path = std::getenv("LWM_TEST_JOURNAL");
            if (!path || std::strlen(path) >= sizeof(address.sun_path))
            {
                errno = EINVAL;
                return -1;
            }
            address.sun_family = AF_UNIX;
            std::strcpy(address.sun_path, path);
            copy.msg_name = &address;
            copy.msg_namelen = sizeof(address);
        }
    }
    return syscall(SYS_sendmsg, fd, &copy, flags);
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "check-fd")
        return fcntl(std::stoi(argv[2]), F_GETFD) < 0 && errno == EBADF ? 0 : 8;
    if (argc == 2 && std::string_view(argv[1]) == "exec-success")
    {
        std::cout << "exec replaced worker\n";
        return 0;
    }
    std::array<bool, 128> inherited{ };
    for (int fd = 3; fd < static_cast<int>(inherited.size()); ++fd) inherited[fd] = fcntl(fd, F_GETFD) >= 0;
    lwm::log::LogOptions options;
    options.target = lwm::log::Target::Stderr;
    std::string mode = "levels";
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--journal")
            options.target = lwm::log::Target::Journal;
        else if (arg == "--plain")
            options.color = lwm::log::ColorMode::Never;
        else if (arg == "--color")
            options.color = lwm::log::ColorMode::Always;
        else if (arg == "--trace")
            options.level = quill::LogLevel::TraceL3;
        else if (arg == "--off")
            options.level = quill::LogLevel::None;
        else
            mode = arg;
    }
    int flags = fcntl(STDERR_FILENO, F_GETFL);
    if (auto result = lwm::log::initialize(options); !result)
    {
        std::cout << result.error() << '\n';
        return 1;
    }
    if (fcntl(STDERR_FILENO, F_GETFL) != flags)
        return 3;
    if (options.level == quill::LogLevel::None
        && std::distance(std::filesystem::directory_iterator("/proc/self/task"), std::filesystem::directory_iterator())
            != 1)
        return 11;
    if (mode == "levels" || mode == "cloexec")
    {
        int evaluated = 0;
        LWM_LOG_DEBUG("disabled {}", ++evaluated);
        LWM_LOG_TRACE("trace");
        LWM_LOG_DEBUG("debug");
        LWM_LOG_INFO("info");
        LWM_LOG_WARN("warn");
        LWM_LOG_ERROR("error");
        LWM_LOG_CRITICAL("critical");
        std::cout << "evaluated=" << evaluated << '\n';
    }
    else if (mode == "strings")
    {
        std::string text = "owned-before-mutation";
        LWM_LOG_INFO("{}", text);
        text = "view-before-mutation";
        LWM_LOG_INFO("{}", std::string_view(text));
        text = "changed";
        LWM_LOG_INFO("{}", std::string("line one\nPRIORITY=0\nline two\0tail", 33));
        LWM_LOG_INFO("{}", std::string(8192, 'x'));
    }
    else if (mode == "burst" || mode == "exec-blocked" || mode == "invariant")
    {
        std::string text(1000, 'x');
        for (int i = 0; i < 100000; ++i) LWM_LOG_INFO("{} {}", i, text);
    }
    else if (mode == "oversized")
    {
        LWM_LOG_INFO("{}", std::string(1000000, 'x'));
        LWM_LOG_INFO("after oversized record");
    }
    else if (mode == "stopped")
    {
        LWM_LOG_INFO("before shutdown");
        lwm::log::shutdown();
        int evaluated = 0;
        LWM_LOG_CRITICAL("disabled while stopped {}", ++evaluated);
        if (evaluated)
            return 4;
    }
    else if (mode == "backend-error")
    {
        LWM_LOG_INFO("{:invalid}", 1);
        LWM_LOG_INFO("after formatting error");
    }
    else if (mode == "rate-limit")
    {
        for (int i = 0; i < 20; ++i)
        {
            LWM_LOG_WARN_LIMIT(std::chrono::milliseconds(50), "recurring warning");
            if (i == 18)
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
    }
    std::cout << "submitted\n" << std::flush;
#ifndef NDEBUG
    if (mode == "invariant")
    {
        prctl(PR_SET_DUMPABLE, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto state = lwm::test::state();
        lwm::test::add(state, 1);
        state.focus(1);
        // Deliberately validate before completion repairs newly ineligible focus.
        state.focus_hints(1, false, false);
        LWM_ASSERT_INVARIANTS(state);
        return 14;
    }
#endif
    if (mode == "exec-blocked")
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        execl("/definitely/missing/lwm-binary", "lwm", nullptr);
        if (errno != ENOENT)
            return 12;
        LWM_LOG_CRITICAL("still logging after failed exec");
        execl(argv[0], argv[0], "exec-success", nullptr);
        return 13;
    }
    lwm::log::shutdown();
    if (mode == "cloexec")
    {
        int checked = 0;
        for (int fd = 3; fd < 128; ++fd)
        {
            if (inherited[fd] || fcntl(fd, F_GETFD) < 0)
                continue;
            if (!(fcntl(fd, F_GETFD) & FD_CLOEXEC))
                return 6;
            ++checked;
            std::string number = std::to_string(fd);
            pid_t child = fork();
            if (child == 0)
            {
                execl(argv[0], argv[0], "check-fd", number.c_str(), nullptr);
                _exit(9);
            }
            int status;
            if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
                return 7;
        }
        if (!checked)
            return 10;
    }
    std::cout << lwm::log::status_json() << '\n';
}
