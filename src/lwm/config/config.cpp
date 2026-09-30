#include "config.hpp"
#include <X11/Xlib.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <regex>
#include <set>
#include <string_view>
#include <toml++/toml.hpp>

namespace lwm {

namespace {

template <typename T> using ParseResult = std::expected<T, std::string>;

using ParseVoid = std::expected<void, std::string>;

#define LWM_TRY(expr)                                        \
    do {                                                     \
        auto lwm_try_result_ = (expr);                       \
        if (!lwm_try_result_)                                \
            return std::unexpected(lwm_try_result_.error()); \
    } while (false)

#define LWM_TRYV(name, expr)                           \
    auto name##_result = (expr);                       \
    if (!name##_result)                                \
        return std::unexpected(name##_result.error()); \
    auto name = std::move(*name##_result)

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

bool is_allowed_key(std::string_view key, std::initializer_list<std::string_view> allowed)
{
    return std::ranges::find(allowed, key) != allowed.end();
}

ParseVoid ensure_optional_table(toml::table const& root, std::string_view key, std::string const& context)
{
    if (auto const* node = root.get(key); node && !node->is_table())
        return std::unexpected(context + " must be a table");
    return {};
}

ParseVoid ensure_optional_array(toml::table const& root, std::string_view key, std::string const& context)
{
    if (auto const* node = root.get(key); node && !node->is_array())
        return std::unexpected(context + " must be an array");
    return {};
}

ParseVoid reject_unknown_keys(
    toml::table const& table,
    std::initializer_list<std::string_view> allowed,
    std::string const& context
)
{
    for (auto const& [key, _] : table)
    {
        if (!is_allowed_key(key.str(), allowed))
        {
            return std::unexpected(context + " has unknown key '" + std::string(key.str()) + "'");
        }
    }
    return {};
}

ParseResult<std::string> expect_string(toml::node const& node, std::string const& context)
{
    if (auto value = node.value<std::string>())
        return *value;
    return std::unexpected(context + " must be a string");
}

ParseResult<int64_t> expect_integer(toml::node const& node, std::string const& context)
{
    if (auto value = node.value<int64_t>())
        return *value;
    return std::unexpected(context + " must be an integer");
}

ParseResult<double> expect_number(toml::node const& node, std::string const& context)
{
    if (auto value = node.value<double>(); value && std::isfinite(*value))
        return *value;
    return std::unexpected(context + " must be a finite number");
}

ParseResult<bool> expect_bool(toml::node const& node, std::string const& context)
{
    if (auto value = node.value<bool>())
        return *value;
    return std::unexpected(context + " must be a boolean");
}

ParseResult<toml::table const*> expect_table(toml::node const& node, std::string const& context)
{
    if (auto value = node.as_table())
        return value;
    return std::unexpected(context + " must be a table");
}

ParseResult<toml::array const*> expect_array(toml::node const& node, std::string const& context)
{
    if (auto value = node.as_array())
        return value;
    return std::unexpected(context + " must be an array");
}

ParseResult<std::vector<std::string>> parse_string_array(toml::node const& node, std::string const& context)
{
    LWM_TRYV(array, expect_array(node, context));
    std::vector<std::string> values;
    values.reserve(array->size());
    for (size_t i = 0; i < array->size(); ++i)
    {
        auto const* item = array->get(i);
        if (!item)
            return std::unexpected(context + "[" + std::to_string(i) + "] is missing");
        LWM_TRYV(value, expect_string(*item, context + "[" + std::to_string(i) + "]"));
        values.push_back(std::move(value));
    }
    return values;
}

ParseResult<std::optional<std::string>>
parse_optional_string(toml::table const& table, std::string_view key, std::string const& context)
{
    if (auto const* node = table.get(key))
    {
        LWM_TRYV(value, expect_string(*node, context + "." + std::string(key)));
        return std::optional<std::string>(std::move(value));
    }
    return std::optional<std::string>{};
}

ParseResult<std::optional<bool>>
parse_optional_bool(toml::table const& table, std::string_view key, std::string const& context)
{
    if (auto const* node = table.get(key))
    {
        LWM_TRYV(value, expect_bool(*node, context + "." + std::string(key)));
        return std::optional<bool>(value);
    }
    return std::optional<bool>{};
}

ParseResult<std::optional<int64_t>>
parse_optional_integer(toml::table const& table, std::string_view key, std::string const& context)
{
    if (auto const* node = table.get(key))
    {
        LWM_TRYV(value, expect_integer(*node, context + "." + std::string(key)));
        return std::optional<int64_t>(value);
    }
    return std::optional<int64_t>{};
}

ParseResult<std::optional<double>>
parse_optional_number(toml::table const& table, std::string_view key, std::string const& context)
{
    if (auto const* node = table.get(key))
    {
        LWM_TRYV(value, expect_number(*node, context + "." + std::string(key)));
        return std::optional<double>(value);
    }
    return std::optional<double>{};
}

ParseResult<WindowType> parse_rule_type(std::string type, std::string const& context)
{
    std::ranges::transform(type, type.begin(), [](unsigned char c) { return std::tolower(c); });
    if (type == "desktop")
        return WindowType::Desktop;
    if (type == "dock")
        return WindowType::Dock;
    if (type == "toolbar")
        return WindowType::Toolbar;
    if (type == "menu")
        return WindowType::Menu;
    if (type == "utility")
        return WindowType::Utility;
    if (type == "splash")
        return WindowType::Splash;
    if (type == "dialog")
        return WindowType::Dialog;
    if (type == "dropdown_menu" || type == "dropdownmenu")
        return WindowType::DropdownMenu;
    if (type == "popup_menu" || type == "popupmenu")
        return WindowType::PopupMenu;
    if (type == "tooltip")
        return WindowType::Tooltip;
    if (type == "notification")
        return WindowType::Notification;
    if (type == "combo")
        return WindowType::Combo;
    if (type == "dnd")
        return WindowType::Dnd;
    if (type == "normal")
        return WindowType::Normal;

    return std::unexpected(context + " has unknown window type '" + type + "'");
}

ParseVoid parse_regex_matchers(toml::table const& table, std::string const& context, WindowMatcher& match)
{
    auto parse_pattern = [&](std::string_view key, std::optional<std::regex>& field) -> ParseVoid
    {
        LWM_TRYV(pattern, parse_optional_string(table, key, context));
        if (!pattern)
            return {};

        if (pattern->empty())
            return std::unexpected(context + "." + std::string(key) + " must not be empty");
        try
        {
            field.emplace("^(?:" + *pattern + ")$", std::regex::ECMAScript | std::regex::optimize);
        }
        catch (std::regex_error const& error)
        {
            return std::unexpected(context + "." + std::string(key) + " has invalid regex: " + error.what());
        }
        return {};
    };

    LWM_TRY(parse_pattern("class", match.class_regex));
    LWM_TRY(parse_pattern("instance", match.instance_regex));
    LWM_TRY(parse_pattern("title", match.title_regex));
    return {};
}

bool has_workspace_name(Config const& config, std::string_view workspace_name)
{
    return std::ranges::find(config.workspaces.names, workspace_name) != config.workspaces.names.end();
}

bool has_scratchpad_name(Config const& config, std::string_view scratchpad_name)
{
    return std::ranges::any_of(
        config.scratchpads,
        [&](ScratchpadConfig const& scratchpad) { return scratchpad.name == scratchpad_name; }
    );
}

ParseResult<uint16_t> parse_modifiers(std::string_view text, std::string const& context)
{
    uint16_t result = 0;
    if (text.empty())
        return result;
    for (size_t start = 0;;)
    {
        auto end = text.find('+', start);
        auto token = text.substr(start, end == text.npos ? end : end - start);
        uint16_t mask = 0;
        if (token == "super")
            mask = XCB_MOD_MASK_4;
        else if (token == "shift")
            mask = XCB_MOD_MASK_SHIFT;
        else if (token == "ctrl" || token == "control")
            mask = XCB_MOD_MASK_CONTROL;
        else if (token == "alt")
            mask = XCB_MOD_MASK_1;
        else
            return std::unexpected(context + " has unknown or empty modifier '" + std::string(token) + "'");
        if (result & mask)
            return std::unexpected(context + " repeats modifier '" + std::string(token) + "'");
        result |= mask;
        if (end == text.npos)
            return result;
        start = end + 1;
    }
}

ParseResult<xcb_keysym_t> parse_keysym(std::string const& key, std::string const& context)
{
    auto symbol = XStringToKeysym(key.c_str());
    if (symbol == NoSymbol)
        return std::unexpected(context + " has unknown key '" + key + "'");
    return static_cast<xcb_keysym_t>(symbol);
}

ParseResult<KeyBinding> parse_key_combo(std::string const& combo, std::string const& context)
{
    auto separator = combo.rfind('+');
    LWM_TRYV(
        modifier,
        parse_modifiers(
            separator == combo.npos ? std::string_view{ } : std::string_view(combo).substr(0, separator),
            context
        )
    );
    LWM_TRYV(keysym, parse_keysym(combo.substr(separator == combo.npos ? 0 : separator + 1), context));
    // A leading '+' is an empty modifier, not an unmodified key.
    if (separator == 0)
        return std::unexpected(context + " contains an empty modifier");
    return KeyBinding{ modifier, keysym };
}

ParseVoid expect_enabled_flag(toml::node const& node, std::string const& context)
{
    LWM_TRYV(enabled, expect_bool(node, context));
    if (!enabled)
        return std::unexpected(context + " must be true when present");
    return {};
}

ParseResult<CommandConfig> parse_command_config(
    toml::node const& node,
    std::string const& context,
    std::map<std::string, CommandConfig> const& registry,
    bool allow_ref
)
{
    LWM_TRYV(table, expect_table(node, context));
    LWM_TRY(reject_unknown_keys(
        *table,
        allow_ref ? std::initializer_list<std::string_view>{ "ref", "shell", "argv" }
                  : std::initializer_list<std::string_view>{ "shell", "argv" },
        context
    ));

    bool has_ref = table->contains("ref");
    bool has_shell = table->contains("shell");
    bool has_argv = table->contains("argv");
    int present = static_cast<int>(has_ref) + static_cast<int>(has_shell) + static_cast<int>(has_argv);
    if (present != 1)
    {
        return std::unexpected(context + " must contain exactly one of 'ref', 'shell', or 'argv'");
    }

    if (has_ref)
    {
        LWM_TRYV(ref, expect_string(*table->get("ref"), context + ".ref"));
        auto it = registry.find(ref);
        if (it == registry.end())
            return std::unexpected(context + ".ref points to unknown command '" + ref + "'");
        return it->second;
    }

    if (has_shell)
    {
        LWM_TRYV(shell, expect_string(*table->get("shell"), context + ".shell"));
        if (shell.empty())
            return std::unexpected(context + ".shell must not be empty");
        return CommandConfig::shell_command(std::move(shell));
    }

    LWM_TRYV(argv, parse_string_array(*table->get("argv"), context + ".argv"));
    if (argv.empty())
        return std::unexpected(context + ".argv must not be empty");
    if (argv.front().empty())
        return std::unexpected(context + ".argv[0] must not be empty");
    return CommandConfig::argv_command(std::move(argv));
}

ParseVoid validate_color(int64_t value, std::string const& context)
{
    if (value < 0 || value > 0xFFFFFFFF)
        return std::unexpected(context + " must be in range 0x000000..0xFFFFFFFF");
    return {};
}

ParseVoid validate_workspace_index(int64_t workspace, size_t count, std::string const& context)
{
    if (workspace < 0 || static_cast<size_t>(workspace) >= count)
    {
        return std::unexpected(context + " must be in range 0.." + std::to_string(count == 0 ? 0 : count - 1));
    }
    return {};
}

ParseVoid
add_binding(std::map<KeyBinding, Action>& bindings, KeyBinding binding, Action action, std::string const& context)
{
    if (!bindings.emplace(binding, std::move(action)).second)
        return std::unexpected(context + " duplicates an existing binding");
    return {};
}

bool action_is_workspace_group(Action const& action, std::string_view action_name)
{
    if (action_name == "switch_workspace")
        return std::holds_alternative<SwitchWorkspaceAction>(action);
    if (action_name == "move_to_workspace")
        return std::holds_alternative<MoveToWorkspaceAction>(action);
    return false;
}

// Parameterless actions share one parser and one name-to-value table.
std::pair<std::string_view, Action> const flag_actions[] = {
    {              "kill",             KillAction{ } },
    {     "reload_config",     ReloadConfigAction{ } },
    {           "restart",          RestartAction{ } },
    {  "toggle_workspace",  ToggleWorkspaceAction{ } },
    { "toggle_fullscreen", ToggleFullscreenAction{ } },
    {      "toggle_float",      ToggleFloatAction{ } },
    {        "focus_next",        FocusNextAction{ } },
    {        "focus_prev",        FocusPrevAction{ } },
    {        "ratio_grow",        RatioGrowAction{ } },
    {      "ratio_shrink",      RatioShrinkAction{ } },
    {         "swap_next",         SwapNextAction{ } },
    {         "swap_prev",         SwapPrevAction{ } },
    {  "scratchpad_stash",  ScratchpadStashAction{ } },
    {  "scratchpad_cycle",  ScratchpadCycleAction{ } },
};

ParseResult<Action> parse_bind_action(toml::table const& table, std::string const& context, Config const& config)
{
    std::string_view name;
    toml::node const* value = nullptr;
    for (auto const& [key, node] : table)
    {
        if (key == "key")
            continue;
        if (value)
            return std::unexpected(context + " must define exactly one action");
        name = key.str();
        value = &node;
    }
    if (!value)
        return std::unexpected(context + " must define exactly one action");
    auto field = context + "." + std::string(name);
    for (auto const& [key, action] : flag_actions)
    {
        if (name != key)
            continue;
        LWM_TRY(expect_enabled_flag(*value, field));
        return action;
    }
    if (name == "spawn")
    {
        LWM_TRYV(command, parse_command_config(*value, field, config.commands, true));
        return SpawnAction{ std::move(command) };
    }
    if (name == "switch_workspace" || name == "move_to_workspace")
    {
        LWM_TRYV(workspace, expect_integer(*value, field));
        LWM_TRY(validate_workspace_index(workspace, config.workspaces.count, field));
        return name == "switch_workspace" ? Action{ SwitchWorkspaceAction{ static_cast<size_t>(workspace) } }
                                          : Action{ MoveToWorkspaceAction{ static_cast<size_t>(workspace) } };
    }
    if (name == "focus_monitor" || name == "move_to_monitor")
    {
        LWM_TRYV(direction, expect_integer(*value, field));
        if (direction != -1 && direction != 1)
            return std::unexpected(field + " must be -1 or 1");
        return name == "focus_monitor" ? Action{ FocusMonitorAction{ static_cast<int>(direction) } }
                                       : Action{ MoveToMonitorAction{ static_cast<int>(direction) } };
    }
    if (name == "toggle_scratchpad")
    {
        LWM_TRYV(scratchpad, expect_string(*value, field));
        if (!has_scratchpad_name(config, scratchpad))
            return std::unexpected(field + " points to unknown scratchpad '" + scratchpad + "'");
        return ToggleScratchpadAction{ std::move(scratchpad) };
    }
    return std::unexpected(context + " has unknown key '" + std::string(name) + "'");
}

ParseResult<RuleGeometry> parse_geometry(toml::node const& node, std::string const& context)
{
    LWM_TRYV(table, expect_table(node, context));
    LWM_TRY(reject_unknown_keys(*table, { "x", "y", "width", "height" }, context));

    RuleGeometry geometry;
    LWM_TRYV(x, parse_optional_integer(*table, "x", context));
    if (x)
    {
        if (*x < -32768 || *x > 32767)
            return std::unexpected(context + ".x must be between -32768 and 32767");
        geometry.x = static_cast<int16_t>(*x);
    }
    LWM_TRYV(y, parse_optional_integer(*table, "y", context));
    if (y)
    {
        if (*y < -32768 || *y > 32767)
            return std::unexpected(context + ".y must be between -32768 and 32767");
        geometry.y = static_cast<int16_t>(*y);
    }
    LWM_TRYV(width, parse_optional_integer(*table, "width", context));
    if (width)
    {
        if (*width <= 0 || *width > 65535)
            return std::unexpected(context + ".width must be between 1 and 65535");
        geometry.width = static_cast<uint16_t>(*width);
    }
    LWM_TRYV(height, parse_optional_integer(*table, "height", context));
    if (height)
    {
        if (*height <= 0 || *height > 65535)
            return std::unexpected(context + ".height must be between 1 and 65535");
        geometry.height = static_cast<uint16_t>(*height);
    }

    return geometry;
}

ParseVoid parse_rule_match_table(toml::table const& table, std::string const& context, WindowRuleConfig& rule)
{
    LWM_TRY(reject_unknown_keys(table, { "class", "instance", "title", "type", "transient" }, context));
    LWM_TRY(parse_regex_matchers(table, context, rule.match));
    LWM_TRYV(type, parse_optional_string(table, "type", context));
    if (type)
    {
        LWM_TRYV(parsed_type, parse_rule_type(*type, context + ".type"));
        rule.type = parsed_type;
    }
    LWM_TRYV(transient, parse_optional_bool(table, "transient", context));
    if (transient)
        rule.transient = *transient;

    return {};
}

ParseVoid
parse_scratchpad_match_table(toml::table const& table, std::string const& context, ScratchpadConfig& scratchpad)
{
    LWM_TRY(reject_unknown_keys(table, { "class", "instance", "title" }, context));

    LWM_TRY(parse_regex_matchers(table, context, scratchpad.match));
    if (scratchpad.match.empty())
    {
        return std::unexpected(context + " must define at least one matcher");
    }

    return {};
}

ParseVoid parse_rule_apply_table(
    toml::table const& table,
    std::string const& context,
    WindowRuleConfig& rule,
    Config const& config
)
{
    bool has_action = false;

    LWM_TRY(reject_unknown_keys(
        table,
        {
            "floating",
            "workspace",
            "workspace_name",
            "monitor",
            "monitor_name",
            "fullscreen",
            "above",
            "below",
            "sticky",
            "skip_taskbar",
            "skip_pager",
            "borderless",
            "geometry",
            "center",
            "scratchpad",
        },
        context
    ));

    LWM_TRYV(floating, parse_optional_bool(table, "floating", context));
    if (floating)
    {
        rule.floating = *floating;
        has_action = true;
    }
    LWM_TRYV(workspace, parse_optional_integer(table, "workspace", context));
    if (workspace)
    {
        LWM_TRY(validate_workspace_index(*workspace, config.workspaces.count, context + ".workspace"));
        rule.workspace = static_cast<int>(*workspace);
        has_action = true;
    }
    LWM_TRYV(workspace_name, parse_optional_string(table, "workspace_name", context));
    if (workspace && workspace_name)
        return std::unexpected(context + " cannot define both 'workspace' and 'workspace_name'");
    if (workspace_name)
    {
        if (!has_workspace_name(config, *workspace_name))
            return std::unexpected(context + ".workspace_name points to unknown workspace '" + *workspace_name + "'");
        rule.workspace_name = std::move(*workspace_name);
        has_action = true;
    }
    LWM_TRYV(monitor, parse_optional_integer(table, "monitor", context));
    if (monitor)
    {
        if (*monitor < 0 || *monitor > std::numeric_limits<int>::max())
            return std::unexpected(context + ".monitor must fit a non-negative int");
        rule.monitor = static_cast<int>(*monitor);
        has_action = true;
    }
    LWM_TRYV(monitor_name, parse_optional_string(table, "monitor_name", context));
    if (monitor && monitor_name)
        return std::unexpected(context + " cannot define both 'monitor' and 'monitor_name'");
    if (monitor_name)
    {
        rule.monitor_name = std::move(*monitor_name);
        has_action = true;
    }
    LWM_TRYV(fullscreen, parse_optional_bool(table, "fullscreen", context));
    if (fullscreen)
    {
        rule.fullscreen = *fullscreen;
        has_action = true;
    }
    LWM_TRYV(above, parse_optional_bool(table, "above", context));
    LWM_TRYV(below, parse_optional_bool(table, "below", context));
    if (above && *above && below && *below)
        return std::unexpected(context + " cannot set both 'above' and 'below' to true");
    if (above)
    {
        rule.above = *above;
        has_action = true;
    }
    if (below)
    {
        rule.below = *below;
        has_action = true;
    }
    LWM_TRYV(sticky, parse_optional_bool(table, "sticky", context));
    if (sticky)
    {
        rule.sticky = *sticky;
        has_action = true;
    }
    LWM_TRYV(skip_taskbar, parse_optional_bool(table, "skip_taskbar", context));
    if (skip_taskbar)
    {
        rule.skip_taskbar = *skip_taskbar;
        has_action = true;
    }
    LWM_TRYV(skip_pager, parse_optional_bool(table, "skip_pager", context));
    if (skip_pager)
    {
        rule.skip_pager = *skip_pager;
        has_action = true;
    }
    LWM_TRYV(borderless, parse_optional_bool(table, "borderless", context));
    if (borderless)
    {
        rule.borderless = *borderless;
        has_action = true;
    }
    if (auto const* geometry = table.get("geometry"))
    {
        LWM_TRYV(parsed_geometry, parse_geometry(*geometry, context + ".geometry"));
        rule.geometry = std::move(parsed_geometry);
        has_action = true;
    }
    LWM_TRYV(center, parse_optional_bool(table, "center", context));
    if (center)
    {
        rule.center = *center;
        has_action = true;
    }
    LWM_TRYV(scratchpad_name, parse_optional_string(table, "scratchpad", context));
    if (scratchpad_name)
    {
        if (!has_scratchpad_name(config, *scratchpad_name))
            return std::unexpected(context + ".scratchpad points to unknown scratchpad '" + *scratchpad_name + "'");
        rule.scratchpad = std::move(*scratchpad_name);
        has_action = true;
    }

    if (!has_action)
        return std::unexpected(context + " must define at least one action");

    return {};
}

std::map<KeyBinding, Action> build_default_keybinds(Config const& config)
{
    std::map<KeyBinding, Action> keybinds;

    auto add_spawn = [&](uint16_t mod, std::string key, std::string_view command_name)
    {
        auto it = config.commands.find(std::string(command_name));
        if (it == config.commands.end())
            return;

        keybinds[{ mod, static_cast<xcb_keysym_t>(XStringToKeysym(key.c_str())) }] = SpawnAction{ it->second };
    };

    auto add_workspace_binds = [&](uint16_t mod, std::vector<std::string> const& keys, std::string_view action)
    {
        size_t limit = std::min(config.workspaces.count, keys.size());
        for (size_t i = 0; i < limit; ++i)
        {
            Action binding_action;
            if (action == "switch_workspace")
                binding_action = SwitchWorkspaceAction{ i };
            else
                binding_action = MoveToWorkspaceAction{ i };
            keybinds[{ mod, static_cast<xcb_keysym_t>(XStringToKeysym(keys[i].c_str())) }] = std::move(binding_action);
        }
    };

    add_spawn(XCB_MOD_MASK_4, "Return", "terminal");
    add_spawn(XCB_MOD_MASK_4, "d", "launcher");

    keybinds[{ XCB_MOD_MASK_4, static_cast<xcb_keysym_t>(XStringToKeysym("q")) }] = KillAction{ };
    add_workspace_binds(
        XCB_MOD_MASK_4,
        { "ampersand",
          "eacute",
          "quotedbl",
          "apostrophe",
          "parenleft",
          "minus",
          "egrave",
          "underscore",
          "ccedilla",
          "agrave" },
        "switch_workspace"
    );
    add_workspace_binds(XCB_MOD_MASK_4, { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" }, "switch_workspace");
    add_workspace_binds(
        XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT,
        { "ampersand",
          "eacute",
          "quotedbl",
          "apostrophe",
          "parenleft",
          "minus",
          "egrave",
          "underscore",
          "ccedilla",
          "agrave" },
        "move_to_workspace"
    );
    add_workspace_binds(
        XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT,
        { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
        "move_to_workspace"
    );

    keybinds[{ XCB_MOD_MASK_4, static_cast<xcb_keysym_t>(XStringToKeysym("Left")) }] = FocusMonitorAction{ -1 };
    keybinds[{ XCB_MOD_MASK_4, static_cast<xcb_keysym_t>(XStringToKeysym("Right")) }] = FocusMonitorAction{ 1 };
    keybinds[{ XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, static_cast<xcb_keysym_t>(XStringToKeysym("Left")) }] =
        MoveToMonitorAction{ -1 };
    keybinds[{ XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, static_cast<xcb_keysym_t>(XStringToKeysym("Right")) }] =
        MoveToMonitorAction{ 1 };
    keybinds[{ XCB_MOD_MASK_4, static_cast<xcb_keysym_t>(XStringToKeysym("f")) }] = ToggleFullscreenAction{ };
    keybinds[{ XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, static_cast<xcb_keysym_t>(XStringToKeysym("f")) }] =
        ToggleFloatAction{ };
    keybinds[{ XCB_MOD_MASK_4, static_cast<xcb_keysym_t>(XStringToKeysym("j")) }] = FocusNextAction{ };
    keybinds[{ XCB_MOD_MASK_4, static_cast<xcb_keysym_t>(XStringToKeysym("k")) }] = FocusPrevAction{ };
    keybinds[{ XCB_MOD_MASK_4, static_cast<xcb_keysym_t>(XStringToKeysym("h")) }] = RatioShrinkAction{ };
    keybinds[{ XCB_MOD_MASK_4, static_cast<xcb_keysym_t>(XStringToKeysym("l")) }] = RatioGrowAction{ };

    return keybinds;
}

Config default_values()
{
    Config cfg;
    cfg.workspaces.names = default_workspace_names(cfg.workspaces.count);

    cfg.commands["terminal"] = CommandConfig::argv_command({ "/usr/local/bin/st" });
    cfg.commands["browser"] = CommandConfig::argv_command({ "/usr/bin/firefox" });
    cfg.commands["launcher"] = CommandConfig::argv_command({ "dmenu_run" });

    cfg.mousebinds = {
        { XCB_MOD_MASK_4, 1,     MouseAction::DragWindow },
        { XCB_MOD_MASK_4, 3, MouseAction::ResizeFloating },
        { XCB_MOD_MASK_4, 2,    MouseAction::ToggleFloat },
    };

    return cfg;
}

} // namespace

Config default_config()
{
    auto config = default_values();
    config.keybinds = build_default_keybinds(config);
    return config;
}

ConfigLoadResult load_config_result(std::string const& path)
{
    try
    {
        auto root = toml::parse_file(path);
        LWM_TRY(reject_unknown_keys(
            root,
            {
                "appearance",
                "layout",
                "focus",
                "commands",
                "workspaces",
                "autostart",
                "binds",
                "workspace_binds",
                "mousebinds",
                "rules",
                "scratchpads",
            },
            "top-level config"
        ));
        LWM_TRY(ensure_optional_table(root, "appearance", "[appearance]"));
        LWM_TRY(ensure_optional_table(root, "layout", "[layout]"));
        LWM_TRY(ensure_optional_table(root, "focus", "[focus]"));
        LWM_TRY(ensure_optional_table(root, "commands", "[commands]"));
        LWM_TRY(ensure_optional_table(root, "workspaces", "[workspaces]"));
        LWM_TRY(ensure_optional_table(root, "autostart", "[autostart]"));
        LWM_TRY(ensure_optional_array(root, "binds", "[[binds]]"));
        LWM_TRY(ensure_optional_array(root, "workspace_binds", "[[workspace_binds]]"));
        LWM_TRY(ensure_optional_array(root, "mousebinds", "[[mousebinds]]"));
        LWM_TRY(ensure_optional_array(root, "rules", "[[rules]]"));
        LWM_TRY(ensure_optional_array(root, "scratchpads", "[[scratchpads]]"));

        Config cfg = default_values();
        bool has_bind_overrides = root.contains("binds");

        if (auto appearance = root["appearance"].as_table())
        {
            LWM_TRY(reject_unknown_keys(
                *appearance,
                { "padding", "border_width", "border_color", "urgent_border_color" },
                "[appearance]"
            ));

            LWM_TRYV(padding, parse_optional_integer(*appearance, "padding", "[appearance]"));
            if (padding)
            {
                if (*padding < 0 || *padding > 65535)
                    return std::unexpected("[appearance].padding must be in range 0..65535");
                cfg.appearance.padding = static_cast<uint32_t>(*padding);
            }
            LWM_TRYV(border_width, parse_optional_integer(*appearance, "border_width", "[appearance]"));
            if (border_width)
            {
                if (*border_width < 0 || *border_width > 65535)
                    return std::unexpected("[appearance].border_width must be in range 0..65535");
                cfg.appearance.border_width = static_cast<uint32_t>(*border_width);
            }
            LWM_TRYV(border_color, parse_optional_integer(*appearance, "border_color", "[appearance]"));
            if (border_color)
            {
                LWM_TRY(validate_color(*border_color, "[appearance].border_color"));
                cfg.appearance.border_color = static_cast<uint32_t>(*border_color);
            }
            LWM_TRYV(urgent_border_color, parse_optional_integer(*appearance, "urgent_border_color", "[appearance]"));
            if (urgent_border_color)
            {
                LWM_TRY(validate_color(*urgent_border_color, "[appearance].urgent_border_color"));
                cfg.appearance.urgent_border_color = static_cast<uint32_t>(*urgent_border_color);
            }
        }

        if (auto layout = root["layout"].as_table())
        {
            LWM_TRY(reject_unknown_keys(
                *layout,
                { "strategy", "default_ratio", "min_ratio", "resize_grab_threshold" },
                "[layout]"
            ));

            LWM_TRYV(strategy, parse_optional_string(*layout, "strategy", "[layout]"));
            if (strategy)
            {
                if (!parse_layout_strategy(*strategy))
                {
                    return std::unexpected(
                        "[layout].strategy has unknown value '" + *strategy + "' (expected 'master-stack' or 'monocle')"
                    );
                }
                cfg.layout.strategy = std::move(*strategy);
            }
            LWM_TRYV(min_ratio, parse_optional_number(*layout, "min_ratio", "[layout]"));
            if (min_ratio)
            {
                if (*min_ratio < 0.05 || *min_ratio > 0.45)
                    return std::unexpected("[layout].min_ratio must be in range 0.05..0.45");
                cfg.layout.min_ratio = *min_ratio;
            }
            LWM_TRYV(default_ratio, parse_optional_number(*layout, "default_ratio", "[layout]"));
            if (default_ratio)
            {
                if (*default_ratio < cfg.layout.min_ratio || *default_ratio > (1.0 - cfg.layout.min_ratio))
                {
                    return std::unexpected("[layout].default_ratio must respect min_ratio bounds");
                }
                cfg.layout.default_ratio = *default_ratio;
            }
            LWM_TRYV(resize_grab_threshold, parse_optional_integer(*layout, "resize_grab_threshold", "[layout]"));
            if (resize_grab_threshold)
            {
                if (*resize_grab_threshold < 1 || *resize_grab_threshold > 100)
                    return std::unexpected("[layout].resize_grab_threshold must be in range 1..100");
                cfg.layout.resize_grab_threshold = static_cast<uint32_t>(*resize_grab_threshold);
            }
        }

        if (auto focus = root["focus"].as_table())
        {
            LWM_TRY(reject_unknown_keys(*focus, { "warp_cursor_on_monitor_change" }, "[focus]"));
            LWM_TRYV(warp_cursor, parse_optional_bool(*focus, "warp_cursor_on_monitor_change", "[focus]"));
            if (warp_cursor)
                cfg.focus.warp_cursor_on_monitor_change = *warp_cursor;
        }

        if (auto commands = root["commands"].as_table())
        {
            for (auto const& [name, node] : *commands)
            {
                if (!node.is_table())
                    return std::unexpected("[commands]." + std::string(name.str()) + " must be a table");
                LWM_TRYV(
                    command,
                    parse_command_config(node, "[commands]." + std::string(name.str()), cfg.commands, false)
                );
                cfg.commands[std::string(name.str())] = std::move(command);
            }
        }

        bool workspaces_count_set = false;
        bool workspaces_names_set = false;
        if (auto workspaces = root["workspaces"].as_table())
        {
            LWM_TRY(reject_unknown_keys(*workspaces, { "count", "names" }, "[workspaces]"));

            LWM_TRYV(workspace_count, parse_optional_integer(*workspaces, "count", "[workspaces]"));
            if (workspace_count)
            {
                if (*workspace_count < 1 || *workspace_count > 65535)
                    return std::unexpected("[workspaces].count must be in range 1..65535");
                cfg.workspaces.count = static_cast<size_t>(*workspace_count);
                workspaces_count_set = true;
            }
            if (auto const* names = workspaces->get("names"))
            {
                LWM_TRYV(workspace_names, parse_string_array(*names, "[workspaces].names"));
                if (workspace_names.size() > 65535)
                    return std::unexpected("[workspaces].names cannot contain more than 65535 entries");
                cfg.workspaces.names = std::move(workspace_names);
                workspaces_names_set = true;
            }
        }
        normalize_workspaces_config(cfg.workspaces, workspaces_count_set, workspaces_names_set);

        if (!has_bind_overrides)
            cfg.keybinds = build_default_keybinds(cfg);

        if (auto scratchpads = root["scratchpads"].as_array())
        {
            cfg.scratchpads.clear();
            std::set<std::string> names;
            for (size_t i = 0; i < scratchpads->size(); ++i)
            {
                auto const* item = scratchpads->get(i);
                if (!item)
                    return std::unexpected("[[scratchpads]] entry is missing");
                LWM_TRYV(table, expect_table(*item, "[[scratchpads]]#" + std::to_string(i)));
                std::string context = "[[scratchpads]]#" + std::to_string(i);
                LWM_TRY(reject_unknown_keys(*table, { "name", "spawn", "match", "size" }, context));

                ScratchpadConfig scratchpad;
                if (auto const* name = table->get("name"))
                {
                    LWM_TRYV(scratchpad_name, expect_string(*name, context + ".name"));
                    scratchpad.name = std::move(scratchpad_name);
                }
                if (scratchpad.name.empty())
                    return std::unexpected(context + ".name is required");
                if (!names.insert(scratchpad.name).second)
                    return std::unexpected(context + ".name duplicates scratchpad '" + scratchpad.name + "'");

                if (auto const* spawn = table->get("spawn"))
                {
                    LWM_TRYV(spawn_command, parse_command_config(*spawn, context + ".spawn", cfg.commands, true));
                    scratchpad.spawn = std::move(spawn_command);
                }
                else
                    return std::unexpected(context + ".spawn is required");

                if (auto const* match = table->get("match"))
                {
                    LWM_TRYV(match_table, expect_table(*match, context + ".match"));
                    LWM_TRY(parse_scratchpad_match_table(*match_table, context + ".match", scratchpad));
                }
                else
                {
                    return std::unexpected(context + ".match is required");
                }

                if (auto const* size = table->get("size"))
                {
                    LWM_TRYV(size_table, expect_table(*size, context + ".size"));
                    LWM_TRY(reject_unknown_keys(*size_table, { "width", "height" }, context + ".size"));
                    LWM_TRYV(scratchpad_width, parse_optional_number(*size_table, "width", context + ".size"));
                    if (scratchpad_width)
                    {
                        if (*scratchpad_width < 0.1 || *scratchpad_width > 1.0)
                            return std::unexpected(context + ".size.width must be in range 0.1..1.0");
                        scratchpad.width = *scratchpad_width;
                    }
                    LWM_TRYV(scratchpad_height, parse_optional_number(*size_table, "height", context + ".size"));
                    if (scratchpad_height)
                    {
                        if (*scratchpad_height < 0.1 || *scratchpad_height > 1.0)
                            return std::unexpected(context + ".size.height must be in range 0.1..1.0");
                        scratchpad.height = *scratchpad_height;
                    }
                }

                cfg.scratchpads.push_back(std::move(scratchpad));
            }
        }

        if (auto autostart = root["autostart"].as_table())
        {
            LWM_TRY(reject_unknown_keys(*autostart, { "commands" }, "[autostart]"));
            cfg.autostart.commands.clear();

            if (auto const* commands = autostart->get("commands"))
            {
                LWM_TRYV(command_list, expect_array(*commands, "[autostart].commands"));
                for (size_t i = 0; i < command_list->size(); ++i)
                {
                    auto const* item = command_list->get(i);
                    if (!item)
                        return std::unexpected("[autostart].commands[" + std::to_string(i) + "] is missing");
                    LWM_TRYV(
                        command,
                        parse_command_config(
                            *item,
                            "[autostart].commands[" + std::to_string(i) + "]",
                            cfg.commands,
                            true
                        )
                    );
                    cfg.autostart.commands.push_back(std::move(command));
                }
            }
        }

        if (auto binds = root["binds"].as_array())
        {
            for (size_t i = 0; i < binds->size(); ++i)
            {
                auto const* item = binds->get(i);
                if (!item)
                    return std::unexpected("[[binds]] entry is missing");
                LWM_TRYV(table, expect_table(*item, "[[binds]]#" + std::to_string(i)));
                std::string context = "[[binds]]#" + std::to_string(i);

                auto const* key_node = table->get("key");
                if (!key_node)
                    return std::unexpected(context + ".key is required");
                LWM_TRYV(key_combo, expect_string(*key_node, context + ".key"));
                LWM_TRYV(combo, parse_key_combo(key_combo, context + ".key"));

                LWM_TRYV(action, parse_bind_action(*table, context, cfg));
                LWM_TRY(add_binding(cfg.keybinds, combo, std::move(action), context));
            }
        }

        std::set<std::pair<uint16_t, std::string>> replaced_workspace_groups;
        if (auto workspace_binds = root["workspace_binds"].as_array())
        {
            for (size_t i = 0; i < workspace_binds->size(); ++i)
            {
                auto const* item = workspace_binds->get(i);
                if (!item)
                    return std::unexpected("[[workspace_binds]] entry is missing");
                LWM_TRYV(table, expect_table(*item, "[[workspace_binds]]#" + std::to_string(i)));
                std::string context = "[[workspace_binds]]#" + std::to_string(i);
                LWM_TRY(reject_unknown_keys(*table, { "mode", "mod", "keys" }, context));

                auto const* mode_node = table->get("mode");
                auto const* mod_node = table->get("mod");
                auto const* keys_node = table->get("keys");
                if (!mode_node || !mod_node || !keys_node)
                    return std::unexpected(context + " requires 'mode', 'mod', and 'keys'");

                LWM_TRYV(mode, expect_string(*mode_node, context + ".mode"));
                if (mode != "switch" && mode != "move")
                    return std::unexpected(context + ".mode must be 'switch' or 'move'");

                LWM_TRYV(mod_value, expect_string(*mod_node, context + ".mod"));
                LWM_TRYV(mod, parse_modifiers(mod_value, context + ".mod"));
                LWM_TRYV(keys, parse_string_array(*keys_node, context + ".keys"));
                if (keys.size() != cfg.workspaces.count)
                {
                    return std::unexpected(
                        context + ".keys must contain exactly " + std::to_string(cfg.workspaces.count) + " entries"
                    );
                }

                std::string action = mode == "switch" ? "switch_workspace" : "move_to_workspace";
                if (!has_bind_overrides)
                {
                    auto const [_, inserted] = replaced_workspace_groups.insert({ mod, action });
                    if (inserted)
                    {
                        std::erase_if(
                            cfg.keybinds,
                            [&](auto const& existing)
                            {
                                return existing.first.modifier == mod
                                    && action_is_workspace_group(existing.second, action);
                            }
                        );
                    }
                }

                for (size_t workspace = 0; workspace < keys.size(); ++workspace)
                {
                    auto key_context = context + ".keys[" + std::to_string(workspace) + "]";
                    LWM_TRYV(keysym, parse_keysym(keys[workspace], key_context));
                    Action binding_action = action == "switch_workspace" ? Action{ SwitchWorkspaceAction{ workspace } }
                                                                         : Action{ MoveToWorkspaceAction{ workspace } };
                    LWM_TRY(add_binding(cfg.keybinds, { mod, keysym }, std::move(binding_action), key_context));
                }
            }
        }

        if (auto mousebinds = root["mousebinds"].as_array())
        {
            cfg.mousebinds.clear();
            for (size_t i = 0; i < mousebinds->size(); ++i)
            {
                auto const* item = mousebinds->get(i);
                if (!item)
                    return std::unexpected("[[mousebinds]] entry is missing");
                LWM_TRYV(table, expect_table(*item, "[[mousebinds]]#" + std::to_string(i)));
                std::string context = "[[mousebinds]]#" + std::to_string(i);
                LWM_TRY(reject_unknown_keys(*table, { "mod", "button", "action" }, context));

                MousebindConfig mousebind;
                if (auto const* mod_node = table->get("mod"))
                {
                    LWM_TRYV(mod_value, expect_string(*mod_node, context + ".mod"));
                    LWM_TRYV(mod, parse_modifiers(mod_value, context + ".mod"));
                    mousebind.modifier = mod;
                }
                if (auto const* button_node = table->get("button"))
                {
                    LWM_TRYV(button, expect_integer(*button_node, context + ".button"));
                    if (button <= 0 || button > 255)
                        return std::unexpected(context + ".button must be in range 1..255");
                    mousebind.button = static_cast<uint8_t>(button);
                }
                else
                {
                    return std::unexpected(context + ".button is required");
                }

                if (auto const* action_node = table->get("action"))
                {
                    LWM_TRYV(action, expect_string(*action_node, context + ".action"));
                    if (action == "drag_window")
                        mousebind.action = MouseAction::DragWindow;
                    else if (action == "resize_floating")
                        mousebind.action = MouseAction::ResizeFloating;
                    else if (action == "toggle_float")
                        mousebind.action = MouseAction::ToggleFloat;
                    else
                        return std::unexpected(context + ".action has unknown mouse action '" + action + "'");
                }
                else
                {
                    return std::unexpected(context + ".action is required");
                }

                cfg.mousebinds.push_back(std::move(mousebind));
            }
        }

        if (auto rules = root["rules"].as_array())
        {
            cfg.rules.clear();
            for (size_t i = 0; i < rules->size(); ++i)
            {
                auto const* item = rules->get(i);
                if (!item)
                    return std::unexpected("[[rules]] entry is missing");
                LWM_TRYV(table, expect_table(*item, "[[rules]]#" + std::to_string(i)));
                std::string context = "[[rules]]#" + std::to_string(i);
                LWM_TRY(reject_unknown_keys(*table, { "match", "apply" }, context));

                WindowRuleConfig rule;

                if (auto const* match = table->get("match"))
                {
                    LWM_TRYV(match_table, expect_table(*match, context + ".match"));
                    LWM_TRY(parse_rule_match_table(*match_table, context + ".match", rule));
                }

                auto const* apply = table->get("apply");
                if (!apply)
                    return std::unexpected(context + ".apply is required");
                LWM_TRYV(apply_table, expect_table(*apply, context + ".apply"));
                LWM_TRY(parse_rule_apply_table(*apply_table, context + ".apply", rule, cfg));

                cfg.rules.push_back(std::move(rule));
            }
        }

        return cfg;
    }
    catch (toml::parse_error const& err)
    {
        return std::unexpected("Config parse error in '" + path + "': " + std::string(err.description()));
    }
    catch (std::exception const& e)
    {
        return std::unexpected("Config error in '" + path + "': " + std::string(e.what()));
    }
}

#undef LWM_TRY
#undef LWM_TRYV

} // namespace lwm
