#include "cli.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <lwm/config/config.hpp>
#include <lwm/core/log.hpp>
#include <lwm/wm.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

std::string default_config_path()
{
    if (char const* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/lwm/config.toml";
    if (char const* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/lwm/config.toml";
    return "";
}

int main(int argc, char* argv[])
{
    auto parsed = lwm::cli::parse(argc, argv);
    if (!parsed || parsed->help)
    {
        if (!parsed)
            std::cerr << "lwm: " << parsed.error() << '\n';
        std::cerr << lwm::cli::usage(argc > 0 && argv[0] ? argv[0] : "lwm");
        return parsed ? 0 : 2;
    }
    if (parsed->version)
    {
        std::cout << LWM_VERSION << '\n';
        return 0;
    }
    std::string config_path = parsed->config_path.value_or(default_config_path());
    if (parsed->check_config)
    {
        auto config = lwm::load_config(config_path, true);
        if (!config)
            std::cerr << "lwm: " << config.error() << '\n';
        return config ? 0 : 1;
    }

    // Logging survives failed-exec recovery.
    if (auto log = lwm::log::initialize(parsed->log); !log)
    {
        std::cerr << "lwm: process initialization failed: " << log.error() << '\n';
        return 1;
    }

    // The kernel reaps launched processes; exec clears the flag, so neither launched
    // programs nor a restarted WM inherit it. Children that exited before this, such as
    // during an exec restart, are reaped now.
    struct sigaction reap{ };
    reap.sa_handler = SIG_DFL;
    reap.sa_flags = SA_NOCLDWAIT;
    sigemptyset(&reap.sa_mask);
    sigaction(SIGCHLD, &reap, nullptr);
    while (waitpid(-1, nullptr, WNOHANG) > 0)
    { }

    int exit_code = 0;
    std::string_view phase = "Startup";
    try
    {
        LWM_LOG_INFO(
            "Starting LWM: version={} display={}",
            LWM_VERSION,
            std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "default"
        );

        bool explicit_config = parsed->config_path.has_value();
        char* const self = argv[0];
        // After a failed exec, the old WM keeps its connection until recovery owns the screen.
        std::unique_ptr<lwm::WindowManager> predecessor;

        while (true)
        {
            phase = "Configuration loading";
            LWM_LOG_INFO("Loading config: {}", config_path.empty() ? "defaults" : config_path);
            // LWM is usually the session process: a mistake in the file must not end
            // the session, so startup and exec restart fall back to the defaults.
            auto config = lwm::load_config(config_path, explicit_config);
            if (!config)
            {
                LWM_LOG_CRITICAL("{}; using the default configuration", config.error());
                config = lwm::default_config();
            }

            phase = "WM initialization";
            auto wm = std::make_unique<lwm::WindowManager>(std::move(*config), config_path);
            predecessor.reset();
            phase = "WM event loop";
            auto result = wm->run();
            if (result == lwm::RunResult::Failed)
            {
                exit_code = 1;
                break;
            }
            if (result != lwm::RunResult::Restart)
                break;

            std::string restart_binary = wm->restart_binary();
            // A successor restarts from its argv[0], so that names the binary exec'd.
            argv[0] = restart_binary.empty() ? self : restart_binary.data();
            char const* binary = argv[0];
            LWM_LOG_INFO("Restarting: {}", binary);
            // Exec replaces the worker on success; on failure it remains usable.
            // A stalled log destination delays restart only briefly.
            lwm::log::flush_before_exec(std::chrono::milliseconds(100));
            execvp(binary, argv);
            int exec_errno = errno;

            LWM_LOG_CRITICAL("exec '{}' failed: {}, recovering", binary, std::strerror(exec_errno));
            predecessor = std::move(wm);
        }
    }
    catch (std::exception const& e)
    {
        LWM_LOG_CRITICAL("{} failed: {}", phase, e.what());
        exit_code = 1;
    }

    if (exit_code == 0)
        LWM_LOG_INFO("LWM exiting");
    lwm::log::shutdown();
    // Exit-time helpers, including LeakSanitizer's, must be able to wait for children.
    reap.sa_flags = 0;
    sigaction(SIGCHLD, &reap, nullptr);
    return exit_code;
}
