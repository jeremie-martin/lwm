#include <cstdlib>
#include <filesystem>
#include <lwm/config/config.hpp>
#include <lwm/core/log.hpp>
#include <lwm/wm.hpp>
#include <string>

namespace fs = std::filesystem;

std::string get_config_path(int argc, char* argv[])
{
    // Command line argument takes priority
    if (argc > 1)
    {
        return argv[1];
    }

    // Try XDG_CONFIG_HOME
    if (char const* xdg = std::getenv("XDG_CONFIG_HOME"))
    {
        return std::string(xdg) + "/lwm/config.toml";
    }

    // Fall back to ~/.config
    if (char const* home = std::getenv("HOME"))
    {
        return std::string(home) + "/.config/lwm/config.toml";
    }

    return "";
}

int main(int argc, char* argv[])
{
    lwm::log::init();

    try
    {
        LOG_INFO("Starting LWM window manager");

        std::string config_path = get_config_path(argc, argv);
        lwm::Config config;

        if (!config_path.empty() && fs::exists(config_path))
        {
            LOG_INFO("Loading config from: {}", config_path);
            auto loaded = lwm::load_config(config_path);
            if (loaded)
            {
                config = *loaded;
            }
            else
            {
                LOG_WARN("Failed to load config, using defaults");
                config = lwm::default_config();
            }
        }
        else
        {
            LOG_INFO("No config file found, using defaults");
            config = lwm::default_config();
        }

        lwm::WindowManager wm(std::move(config));
        wm.run();
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
