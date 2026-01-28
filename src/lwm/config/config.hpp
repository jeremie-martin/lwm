#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lwm {

struct RuleGeometry
{
    std::optional<int32_t> x;
    std::optional<int32_t> y;
    std::optional<uint32_t> width;
    std::optional<uint32_t> height;
};

struct WindowRuleConfig
{
    // Matching criteria (all optional, AND logic - all specified must match)
    std::optional<std::string> class_pattern;    // WM_CLASS class name (regex)
    std::optional<std::string> instance_pattern; // WM_CLASS instance name (regex)
    std::optional<std::string> title_pattern;    // Window title (regex)
    std::optional<std::string> type;             // "normal", "dialog", "utility", etc.
    std::optional<bool> transient;               // Match only transient windows

    // Actions
    std::optional<bool> floating;              // Force floating (true) or tiled (false)
    std::optional<int> workspace;              // Target workspace (index)
    std::optional<std::string> workspace_name; // Target workspace (by name)
    std::optional<int> monitor;                // Target monitor (index)
    std::optional<std::string> monitor_name;   // Target monitor (by name like "HDMI-1")
    std::optional<bool> fullscreen;            // Start fullscreen
    std::optional<bool> above;                 // Always on top
    std::optional<bool> below;                 // Always below
    std::optional<bool> sticky;                // Visible on all workspaces
    std::optional<bool> skip_taskbar;          // Exclude from taskbar
    std::optional<bool> skip_pager;            // Exclude from pager
    std::optional<RuleGeometry> geometry;      // Floating geometry
    std::optional<bool> center;
};

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
};

struct ProgramsConfig
{
    std::string terminal = "/usr/local/bin/st";
    std::string browser = "/usr/bin/firefox";
    std::string launcher = "dmenu_run";
};

struct FocusConfig
{
    bool warp_cursor_on_monitor_change = false;
};

struct WorkspacesConfig
{
    size_t count = 10;
    std::vector<std::string> names;
};

struct AutostartConfig
{
    std::vector<std::string> commands;
};

struct Config
{
    AppearanceConfig appearance;
    FocusConfig focus;
    ProgramsConfig programs;
    WorkspacesConfig workspaces;
    AutostartConfig autostart;
    std::vector<KeybindConfig> keybinds;
    std::vector<MousebindConfig> mousebinds;
    std::vector<WindowRuleConfig> rules;
};

std::optional<Config> load_config(std::string const& path);
Config default_config();

} // namespace lwm
