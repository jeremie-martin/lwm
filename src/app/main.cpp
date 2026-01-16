#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <lwm/config/config.hpp>
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
    try
    {
        std::cout << "Starting LWM window manager" << std::endl;

        std::string config_path = get_config_path(argc, argv);
        lwm::Config config;

        if (!config_path.empty() && fs::exists(config_path))
        {
            std::cout << "Loading config from: " << config_path << std::endl;
            auto loaded = lwm::load_config(config_path);
            if (loaded)
            {
                config = *loaded;
            }
            else
            {
                std::cerr << "Failed to load config, using defaults" << std::endl;
                config = lwm::default_config();
            }
        }
        else
        {
            std::cout << "No config file found, using defaults" << std::endl;
            config = lwm::default_config();
        }

        lwm::WindowManager wm(std::move(config));
        wm.run();
    }
    catch (std::exception const& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "LWM exiting" << std::endl;
    return 0;
}
