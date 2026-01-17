#include "config.hpp"
#include <iostream>
#include <toml++/toml.hpp>

namespace lwm {

namespace {

std::vector<std::string> default_workspace_names(size_t count)
{
    std::vector<std::string> names;
    names.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        names.push_back(std::to_string(i + 1));
    }
    return names;
}

void normalize_workspaces_config(WorkspacesConfig& workspaces, bool count_set, bool names_set)
{
    if (!count_set && names_set && !workspaces.names.empty())
    {
        workspaces.count = workspaces.names.size();
    }

    if (workspaces.count < 1)
    {
        workspaces.count = 1;
    }

    if (!names_set || workspaces.names.empty())
    {
        workspaces.names = default_workspace_names(workspaces.count);
        return;
    }

    if (workspaces.names.size() < workspaces.count)
    {
        for (size_t i = workspaces.names.size(); i < workspaces.count; ++i)
        {
            workspaces.names.push_back(std::to_string(i + 1));
        }
    }
    else if (workspaces.names.size() > workspaces.count)
    {
        workspaces.names.resize(workspaces.count);
    }
}

} // namespace

Config default_config()
{
    Config cfg;
    // appearance and programs use struct defaults from config.hpp
    cfg.workspaces.names = default_workspace_names(cfg.workspaces.count);

    cfg.keybinds = {
        {       "super",     "Return",                 "spawn", "terminal", -1 },
        {       "super",          "d",                 "spawn", "launcher", -1 },
        {       "super",          "q",                  "kill",         "", -1 },
        // Workspace switching - AZERTY
        {       "super",  "ampersand",      "switch_workspace",         "",  0 },
        {       "super",     "eacute",      "switch_workspace",         "",  1 },
        {       "super",   "quotedbl",      "switch_workspace",         "",  2 },
        {       "super", "apostrophe",      "switch_workspace",         "",  3 },
        {       "super",  "parenleft",      "switch_workspace",         "",  4 },
        {       "super",      "minus",      "switch_workspace",         "",  5 },
        {       "super",     "egrave",      "switch_workspace",         "",  6 },
        {       "super", "underscore",      "switch_workspace",         "",  7 },
        {       "super",   "ccedilla",      "switch_workspace",         "",  8 },
        {       "super",     "agrave",      "switch_workspace",         "",  9 },
        // Workspace switching - QWERTY
        {       "super",          "1",      "switch_workspace",         "",  0 },
        {       "super",          "2",      "switch_workspace",         "",  1 },
        {       "super",          "3",      "switch_workspace",         "",  2 },
        {       "super",          "4",      "switch_workspace",         "",  3 },
        {       "super",          "5",      "switch_workspace",         "",  4 },
        {       "super",          "6",      "switch_workspace",         "",  5 },
        {       "super",          "7",      "switch_workspace",         "",  6 },
        {       "super",          "8",      "switch_workspace",         "",  7 },
        {       "super",          "9",      "switch_workspace",         "",  8 },
        {       "super",          "0",      "switch_workspace",         "",  9 },
        // Move window to workspace - AZERTY
        { "super+shift",  "ampersand",     "move_to_workspace",         "",  0 },
        { "super+shift",     "eacute",     "move_to_workspace",         "",  1 },
        { "super+shift",   "quotedbl",     "move_to_workspace",         "",  2 },
        { "super+shift", "apostrophe",     "move_to_workspace",         "",  3 },
        { "super+shift",  "parenleft",     "move_to_workspace",         "",  4 },
        { "super+shift",      "minus",     "move_to_workspace",         "",  5 },
        { "super+shift",     "egrave",     "move_to_workspace",         "",  6 },
        { "super+shift", "underscore",     "move_to_workspace",         "",  7 },
        { "super+shift",   "ccedilla",     "move_to_workspace",         "",  8 },
        { "super+shift",     "agrave",     "move_to_workspace",         "",  9 },
        // Move window to workspace - QWERTY
        { "super+shift",          "1",     "move_to_workspace",         "",  0 },
        { "super+shift",          "2",     "move_to_workspace",         "",  1 },
        { "super+shift",          "3",     "move_to_workspace",         "",  2 },
        { "super+shift",          "4",     "move_to_workspace",         "",  3 },
        { "super+shift",          "5",     "move_to_workspace",         "",  4 },
        { "super+shift",          "6",     "move_to_workspace",         "",  5 },
        { "super+shift",          "7",     "move_to_workspace",         "",  6 },
        { "super+shift",          "8",     "move_to_workspace",         "",  7 },
        { "super+shift",          "9",     "move_to_workspace",         "",  8 },
        { "super+shift",          "0",     "move_to_workspace",         "",  9 },
        // Monitor focus switching
        {       "super",       "Left",    "focus_monitor_left",         "", -1 },
        {       "super",      "Right",   "focus_monitor_right",         "", -1 },
        // Move window to adjacent monitor
        { "super+shift",       "Left",  "move_to_monitor_left",         "", -1 },
        { "super+shift",      "Right", "move_to_monitor_right",         "", -1 },
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
            if (auto v = (*appearance)["enable_internal_bar"].value<bool>())
                cfg.appearance.enable_internal_bar = *v;
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

        bool workspaces_count_set = false;
        bool workspaces_names_set = false;
        if (auto workspaces = tbl["workspaces"].as_table())
        {
            if (auto v = (*workspaces)["count"].value<int64_t>())
            {
                cfg.workspaces.count = (*v > 0) ? static_cast<size_t>(*v) : 1;
                workspaces_count_set = true;
            }
            if (auto names = (*workspaces)["names"].as_array())
            {
                cfg.workspaces.names.clear();
                for (auto const& name : *names)
                {
                    if (auto v = name.value<std::string>())
                        cfg.workspaces.names.push_back(*v);
                }
                workspaces_names_set = true;
            }
        }
        normalize_workspaces_config(cfg.workspaces, workspaces_count_set, workspaces_names_set);

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
                    if (auto v = (*kb)["workspace"].value<int64_t>())
                        keybind.workspace = static_cast<int>(*v);
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
