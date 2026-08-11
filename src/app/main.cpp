#include "cli.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <lwm/config/config.hpp>
#include <lwm/core/log.hpp>
#include <lwm/wm.hpp>
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
        LOG_INFO("No config file selected, using defaults");
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
        LOG_INFO("No config file found, using defaults");
        return lwm::default_config();
    }

    LOG_INFO("Loading config from: {}", config_path);
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

    std::vector<char*> restart_argv;
    restart_argv.reserve(parsed->restart_argv.size() + 1);
    for (std::string& argument : parsed->restart_argv)
        restart_argv.push_back(argument.data());
    restart_argv.push_back(nullptr);

    try
    {
        LOG_INFO("Starting LWM window manager");

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
                lwm::WindowManager wm(std::move(config), config_path);
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
                LOG_ERROR("WM initialization failed, retrying ({}/3): {}", recovery_failures, e.what());
                continue;
            }

            std::string binary = restart_binary.empty() ? parsed->restart_argv.front() : restart_binary;
            LOG_INFO("Restarting: {}", binary);
            auto saved_options = lwm::log::prepare_exec();
            execvp(binary.c_str(), restart_argv.data());
            int exec_errno = errno;

            auto restore_result = lwm::log::restore(saved_options);
            if (!restore_result)
                std::fprintf(stderr, "lwm: failed to restore logging after exec: %s\n", restore_result.error().c_str());
            LOG_CRITICAL("exec '{}' failed: {}, recovering", binary, std::strerror(exec_errno));
        }

        if (runtime_failure)
        {
            lwm::log::shutdown();
            return 1;
        }
    }
    catch (std::exception const& e)
    {
        LOG_CRITICAL("Error: {}", e.what());
        lwm::log::shutdown();
        return 1;
    }

    LOG_INFO("LWM exiting");
    lwm::log::shutdown();
    return 0;
}
