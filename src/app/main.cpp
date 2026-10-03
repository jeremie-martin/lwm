#include "cli.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <lwm/config/config.hpp>
#include <lwm/core/log.hpp>
#include <lwm/wm.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

std::string default_config_path()
{
    if (char const* xdg = std::getenv("XDG_CONFIG_HOME"))
        return std::string(xdg) + "/lwm/config.toml";
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

    // Signals precede logging and outlive it; both survive failed-exec recovery.
    std::optional<lwm::SignalPipe> signals;
    try
    {
        signals.emplace();
        if (auto log = lwm::log::initialize(parsed->log); !log)
            throw std::runtime_error(log.error());
    }
    catch (std::exception const& error)
    {
        std::cerr << "lwm: process initialization failed: " << error.what() << '\n';
        return 1;
    }

    int exit_code = 0;
    std::string_view phase = "Startup";
    try
    {
        std::vector<char*> restart_argv;
        restart_argv.reserve(parsed->restart_argv.size() + 1);
        for (std::string& argument : parsed->restart_argv) restart_argv.push_back(argument.data());
        restart_argv.push_back(nullptr);

        LWM_LOG_INFO(
            "Starting LWM: version={} display={}",
            LWM_VERSION,
            std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "default"
        );

        std::string config_path = parsed->config_path.value_or(default_config_path());
        bool explicit_config = parsed->config_path.has_value();

        while (true)
        {
            phase = "Configuration loading";
            LWM_LOG_INFO("Loading config: {}", config_path.empty() ? "defaults" : config_path);
            auto config = lwm::load_config(config_path, explicit_config);
            if (!config)
                throw std::runtime_error(config.error());

            std::string restart_binary;
            {
                phase = "WM initialization";
                lwm::WindowManager wm(std::move(*config), *signals, config_path);
                phase = "WM event loop";
                auto result = wm.run();
                if (result == lwm::RunResult::Failed)
                {
                    exit_code = 1;
                    break;
                }
                if (result != lwm::RunResult::Restart)
                    break;

                phase = "Restart preparation";
                restart_binary = wm.restart_binary();
                wm.prepare_restart();
            }
            std::string binary = restart_binary.empty() ? parsed->restart_argv.front() : restart_binary;
            LWM_LOG_INFO("Restarting: {}", binary);
            // Exec replaces the worker on success; on failure it remains usable.
            // Do not wait for a stalled log destination before restarting.
            execvp(binary.c_str(), restart_argv.data());
            int exec_errno = errno;

            LWM_LOG_CRITICAL("exec '{}' failed: {}, recovering", binary, std::strerror(exec_errno));
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
    return exit_code;
}
