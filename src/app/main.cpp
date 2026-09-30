#include "cli.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <lwm/config/config.hpp>
#include <lwm/core/log.hpp>
#include <lwm/wm.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

std::string default_config_path()
{
    if (char const* xdg = std::getenv("XDG_CONFIG_HOME"))
        return std::string(xdg) + "/lwm/config.toml";
    return "";
}

lwm::Config load_config(std::string const& config_path, bool explicit_path)
{
    if (config_path.empty())
    {
        if (explicit_path)
            throw std::runtime_error("Explicit config path is empty");
        LWM_LOG_INFO("No config file selected, using defaults");
        return lwm::default_config();
    }

    std::error_code error;
    bool exists = fs::exists(config_path, error);
    if (error)
        throw std::runtime_error("Cannot inspect config file '" + config_path + "': " + error.message());
    if (!exists)
    {
        if (explicit_path)
            throw std::runtime_error("Config file not found: " + config_path);
        LWM_LOG_INFO("No config file found, using defaults");
        return lwm::default_config();
    }

    LWM_LOG_INFO("Loading config from: {}", config_path);
    auto loaded = lwm::load_config_result(config_path);
    if (!loaded)
        throw std::runtime_error(loaded.error());
    return *loaded;
}

int main(int argc, char* argv[])
{
    auto parsed = lwm::cli::parse(argc, argv);
    if (!parsed)
    {
        std::cerr << "lwm: " << parsed.error() << '\n';
        std::cerr << lwm::cli::usage(argc > 0 && argv[0] ? argv[0] : "lwm");
        return 2;
    }
    if (parsed->help)
    {
        std::cerr << lwm::cli::usage(argc > 0 && argv[0] ? argv[0] : "lwm");
        return 0;
    }
    if (parsed->version)
    {
        std::cout << LWM_VERSION << '\n';
        return 0;
    }

    std::optional<lwm::SignalPipe> signals;
    try
    {
        signals.emplace();
    }
    catch (std::exception const& error)
    {
        std::cerr << "lwm: signal initialization failed: " << error.what() << '\n';
        return 1;
    }

    try
    {
        auto log_init = lwm::log::initialize(parsed->log);
        if (!log_init)
        {
            std::cerr << "lwm: logging initialization failed: " << log_init.error() << '\n';
            return 1;
        }
    }
    catch (std::exception const& error)
    {
        std::cerr << "lwm: logging initialization failed: " << error.what() << '\n';
        return 1;
    }

    try
    {
        std::vector<char*> restart_argv;
        restart_argv.reserve(parsed->restart_argv.size() + 1);
        for (std::string& argument : parsed->restart_argv) restart_argv.push_back(argument.data());
        restart_argv.push_back(nullptr);

        LWM_LOG_INFO("Starting LWM window manager");

        std::string config_path = parsed->config_path.value_or(default_config_path());
        bool explicit_config = parsed->config_path.has_value();
        int recovery_failures = 0;
        bool runtime_failure = false;

        while (true)
        {
            lwm::Config config = load_config(config_path, explicit_config);

            std::string restart_binary;
            try
            {
                lwm::WindowManager wm(std::move(config), *signals, config_path);
                recovery_failures = 0;
                auto result = wm.run();
                if (result == lwm::RunResult::Failed)
                {
                    runtime_failure = true;
                    break;
                }
                if (result != lwm::RunResult::Restart)
                    break;

                restart_binary = wm.restart_binary();
                wm.prepare_restart();
            }
            catch (std::exception const& e)
            {
                ++recovery_failures;
                if (recovery_failures >= 3)
                {
                    throw std::runtime_error(
                        "WM initialization failed " + std::to_string(recovery_failures) + " times, giving up: " + e.what()
                    );
                }
                LWM_LOG_ERROR("WM initialization failed, retrying ({}/3): {}", recovery_failures, e.what());
                continue;
            }

            std::string binary = restart_binary.empty() ? parsed->restart_argv.front() : restart_binary;
            LWM_LOG_INFO("Restarting: {}", binary);
            // Exec replaces the worker on success; on failure it remains usable.
            // Do not wait for a stalled log destination before restarting.
            execvp(binary.c_str(), restart_argv.data());
            int exec_errno = errno;

            LWM_LOG_CRITICAL("exec '{}' failed: {}, recovering", binary, std::strerror(exec_errno));
        }

        if (runtime_failure)
        {
            lwm::log::shutdown();
            return 1;
        }
    }
    catch (std::exception const& e)
    {
        LWM_LOG_CRITICAL("Error: {}", e.what());
        lwm::log::shutdown();
        return 1;
    }

    LWM_LOG_INFO("LWM exiting");
    lwm::log::shutdown();
    return 0;
}
