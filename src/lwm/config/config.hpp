#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lwm {

struct KeybindConfig
{
    std::string mod;
    std::string key;
    std::string action;
    std::string command;
    int workspace = -1;
};

struct MousebindConfig
{
    std::string mod;
    int button = 0;
    std::string action;
};

struct AppearanceConfig
{
    uint32_t padding = 10;
    uint32_t border_width = 2;
    uint32_t border_color = 0xFF0000;
    uint32_t status_bar_height = 30;
    uint32_t status_bar_color = 0x808080;
    bool enable_internal_bar = false;
};

struct ProgramsConfig
{
    std::string terminal = "/usr/local/bin/st";
    std::string browser = "/usr/bin/firefox";
    std::string launcher = "dmenu_run";
};

struct WorkspacesConfig
{
    size_t count = 10;
    std::vector<std::string> names;
};

struct Config
{
    AppearanceConfig appearance;
    ProgramsConfig programs;
    WorkspacesConfig workspaces;
    std::vector<KeybindConfig> keybinds;
    std::vector<MousebindConfig> mousebinds;
};

std::optional<Config> load_config(std::string const& path);
Config default_config();

} // namespace lwm
