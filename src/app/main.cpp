#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <lwm/config/config.hpp>
#include <lwm/core/log.hpp>
#include <lwm/wm.hpp>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

std::string get_config_path(int argc, char* argv[])
{
    if (argc > 1)
        return argv[1];
    if (char const* xdg = std::getenv("XDG_CONFIG_HOME"))
    {
        return std::string(xdg) + "/lwm/config.toml";
    }
    return "";
}

lwm::Config load_config(std::string const& config_path)
{
    if (!config_path.empty() && fs::exists(config_path))
    {
        LOG_INFO("Loading config from: {}", config_path);
        auto loaded = lwm::load_config_result(config_path);
        if (loaded)
            return *loaded;
        LOG_WARN("{}; using defaults", loaded.error());
    }
    else
    {
        LOG_INFO("No config file found, using defaults");
    }
    return lwm::default_config();
}

int main(int argc, char* argv[])
{
    lwm::log::init();

    try
    {
        LOG_INFO("Starting LWM window manager");

        std::string config_path = get_config_path(argc, argv);
        int recovery_failures = 0;

        while (true)
        {
            lwm::Config config = load_config(config_path);

            std::string restart_binary;
            try
            {
                lwm::WindowManager wm(std::move(config), config_path);
                recovery_failures = 0; // Reset on successful construction
                auto result = wm.run();
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
                    LOG_ERROR("WM initialization failed {} times, giving up: {}", recovery_failures, e.what());
                    throw;
                }
                LOG_ERROR("WM initialization failed, retrying ({}/3): {}", recovery_failures, e.what());
                continue;
            }
            // WindowManager is destroyed here. The X connection closes with
            // RetainPermanent set, so no resources are destroyed.

            std::string binary = restart_binary.empty() ? std::string(argv[0]) : restart_binary;
            LOG_INFO("Restarting: {}", binary);
            execvp(binary.c_str(), argv);

            // exec failed — recover by looping back and creating a new WM.
            // The restart state is already serialized to X properties, so the
            // new WM instance will re-adopt all windows automatically.
            LOG_ERROR("exec failed: {}, recovering", std::strerror(errno));
        }
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("Error: {}", e.what());
        lwm::log::shutdown();
        return 1;
    }

    LOG_INFO("LWM exiting");
    lwm::log::shutdown();
    return 0;
}
