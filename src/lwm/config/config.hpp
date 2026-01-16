#pragma once

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

struct AppearanceConfig
{
    uint32_t padding = 10;
    uint32_t border_width = 2;
    uint32_t border_color = 0xFF0000;
    uint32_t status_bar_height = 30;
    uint32_t status_bar_color = 0x808080;
};

struct ProgramsConfig
{
    std::string terminal = "/usr/local/bin/st";
    std::string browser = "/usr/bin/firefox";
    std::string launcher = "dmenu_run";
};

struct Config
{
    AppearanceConfig appearance;
    ProgramsConfig programs;
    std::vector<KeybindConfig> keybinds;

    static constexpr int NUM_WORKSPACES = 10;
};

std::optional<Config> load_config(std::string const& path);
Config default_config();

} // namespace lwm
