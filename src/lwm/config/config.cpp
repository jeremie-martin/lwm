#include "config.hpp"
#include <iostream>
#include <toml++/toml.hpp>

namespace lwm
{

Config default_config()
{
    Config cfg;

    cfg.appearance.padding = 10;
    cfg.appearance.border_width = 2;
    cfg.appearance.border_color = 0xFF0000;
    cfg.appearance.status_bar_height = 30;
    cfg.appearance.status_bar_color = 0x808080;

    cfg.programs.terminal = "/usr/local/bin/st";
    cfg.programs.browser = "/usr/bin/firefox";
    cfg.programs.launcher = "dmenu_run";

    // Default AZERTY keybinds
    cfg.keybinds = {
        { "super", "Return", "spawn", "terminal", -1 },
        { "super", "f", "spawn", "browser", -1 },
        { "super", "d", "spawn", "launcher", -1 },
        { "super", "q", "kill", "", -1 },
        // AZERTY number keys for tag switching
        { "super", "ampersand", "switch_tag", "", 0 },
        { "super", "eacute", "switch_tag", "", 1 },
        { "super", "quotedbl", "switch_tag", "", 2 },
        { "super", "apostrophe", "switch_tag", "", 3 },
        { "super", "parenleft", "switch_tag", "", 4 },
        { "super", "minus", "switch_tag", "", 5 },
        { "super", "egrave", "switch_tag", "", 6 },
        { "super", "underscore", "switch_tag", "", 7 },
        { "super", "ccedilla", "switch_tag", "", 8 },
        { "super", "agrave", "switch_tag", "", 9 },
        // Move window to next tag
        { "super+shift", "m", "move_to_tag", "", -1 },
    };

    return cfg;
}

std::optional<Config> load_config(std::string const& path)
{
    try
    {
        auto tbl = toml::parse_file(path);
        Config cfg = default_config();

        // Appearance
        if (auto appearance = tbl["appearance"].as_table())
        {
            if (auto v = (*appearance)["padding"].value<int64_t>())
                cfg.appearance.padding = static_cast<uint32_t>(*v);
            if (auto v = (*appearance)["border_width"].value<int64_t>())
                cfg.appearance.border_width = static_cast<uint32_t>(*v);
            if (auto v = (*appearance)["border_color"].value<int64_t>())
                cfg.appearance.border_color = static_cast<uint32_t>(*v);
            if (auto v = (*appearance)["status_bar_height"].value<int64_t>())
                cfg.appearance.status_bar_height = static_cast<uint32_t>(*v);
            if (auto v = (*appearance)["status_bar_color"].value<int64_t>())
                cfg.appearance.status_bar_color = static_cast<uint32_t>(*v);
        }

        // Programs
        if (auto programs = tbl["programs"].as_table())
        {
            if (auto v = (*programs)["terminal"].value<std::string>())
                cfg.programs.terminal = *v;
            if (auto v = (*programs)["browser"].value<std::string>())
                cfg.programs.browser = *v;
            if (auto v = (*programs)["launcher"].value<std::string>())
                cfg.programs.launcher = *v;
        }

        // Keybinds
        if (auto keybinds = tbl["keybinds"].as_array())
        {
            cfg.keybinds.clear();
            for (auto const& item : *keybinds)
            {
                if (auto kb = item.as_table())
                {
                    KeybindConfig keybind;
                    if (auto v = (*kb)["mod"].value<std::string>())
                        keybind.mod = *v;
                    if (auto v = (*kb)["key"].value<std::string>())
                        keybind.key = *v;
                    if (auto v = (*kb)["action"].value<std::string>())
                        keybind.action = *v;
                    if (auto v = (*kb)["command"].value<std::string>())
                        keybind.command = *v;
                    if (auto v = (*kb)["tag"].value<int64_t>())
                        keybind.tag = static_cast<int>(*v);
                    cfg.keybinds.push_back(keybind);
                }
            }
        }

        return cfg;
    }
    catch (toml::parse_error const& err)
    {
        std::cerr << "Config parse error: " << err.description() << std::endl;
        return std::nullopt;
    }
    catch (std::exception const& e)
    {
        std::cerr << "Config error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

} // namespace lwm
