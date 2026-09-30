#include "lwm/core/log.hpp"
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv)
{
    if (argc > 2 && std::string_view(argv[1]) == "check-fd")
        return fcntl(std::stoi(argv[2]), F_GETFD) < 0 && errno == EBADF ? 0 : 8;
    lwm::log::LogOptions options;
    std::string mode = "levels";
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--stderr")
            options.target = lwm::log::Target::Stderr;
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
    auto result = lwm::log::initialize(options);
    if (!result)
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
    if (mode == "levels")
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
        text.assign("changed");
        LWM_LOG_INFO("{}", std::string("line one\nPRIORITY=0\nline two\0tail", 33));
        LWM_LOG_INFO("{}", std::string(1000000, 'x'));
        LWM_LOG_INFO(
            "{}{}{}{}{}",
            std::string(1024, 'y'),
            std::string(1024, 'y'),
            std::string(1024, 'y'),
            std::string(1024, 'y'),
            std::string(1024, 'y')
        );
    }
    else if (mode == "boundaries")
    {
        char raw[3] = { 'r', 'a', 'w' };
        char const* empty = nullptr;
        LWM_LOG_INFO("{}|{}|{}", raw, empty, std::string_view{ });
        std::string text(1025, 'c');
        LWM_LOG_INFO("{}", text.c_str());
        text = "view before mutation";
        LWM_LOG_INFO("{}", std::string_view(text));
        text = "after";
    }
    else if (mode == "burst")
    {
        std::string text(1000, 'x');
        for (int i = 0; i < 100000; ++i) LWM_LOG_INFO("{} {}", i, text);
    }
    else if (mode == "restore")
    {
        LWM_LOG_INFO("before exec");
        auto saved = lwm::log::prepare_exec();
        LWM_LOG_INFO("disabled while stopped");
        if (!lwm::log::restore(saved))
            return 4;
        LWM_LOG_INFO("after failed exec");
    }
    else if (mode == "failed-restore")
    {
        LWM_LOG_INFO("before failed restore");
        auto saved = lwm::log::prepare_exec();
        setenv("LWM_LOG_SOCKET", "relative", 1);
        if (lwm::log::restore(saved))
            return 12;
        LWM_LOG_CRITICAL("disabled after failed restore");
    }
    else if (mode == "cloexec")
    {
        int checked = 0;
        for (int fd = 3; fd < 128; ++fd)
        {
            if (fcntl(fd, F_GETFD) < 0)
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
    else if (mode == "paced" || mode == "paced-large")
    {
        for (int i = 0; i < 30; ++i)
        {
            if (mode == "paced-large")
                LWM_LOG_INFO(
                    "paced {} {} {} {} {} END",
                    i,
                    std::string(900, 'a'),
                    std::string(900, 'b'),
                    std::string(900, 'c'),
                    std::string(900, 'd')
                );
            else
                LWM_LOG_INFO("paced {}", i);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    lwm::log::shutdown();
    std::cout << lwm::log::status_json() << '\n';
}
