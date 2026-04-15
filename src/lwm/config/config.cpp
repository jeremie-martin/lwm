#include "config.hpp"
#include "lwm/keybind/keybind.hpp"
#include <algorithm>
#include <array>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <toml++/toml.hpp>

namespace lwm {

namespace {

template<typename T>
using ParseResult = std::expected<T, std::string>;

using ParseVoid = std::expected<void, std::string>;

#define LWM_TRY(expr)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        auto lwm_try_result_ = (expr);                                                                                 \
        if (!lwm_try_result_)                                                                                          \
            return std::unexpected(lwm_try_result_.error());                                                           \
    } while (false)

#define LWM_TRYV(name, expr)                                                                                           \
    auto name##_result = (expr);                                                                                       \
    if (!name##_result)                                                                                                \
        return std::unexpected(name##_result.error());                                                                 \
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
    if (auto value = node.value<double>())
        return *value;
    if (auto value = node.value<int64_t>())
        return static_cast<double>(*value);
    return std::unexpected(context + " must be a number");
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

ParseResult<std::optional<std::string>> parse_optional_string(
    toml::table const& table, std::string_view key, std::string const& context)
{
    if (auto const* node = table.get(key))
    {
        LWM_TRYV(value, expect_string(*node, context + "." + std::string(key)));
        return std::optional<std::string>(std::move(value));
    }
    return std::optional<std::string>{};
}

ParseResult<std::optional<bool>> parse_optional_bool(
    toml::table const& table, std::string_view key, std::string const& context)
{
    if (auto const* node = table.get(key))
    {
        LWM_TRYV(value, expect_bool(*node, context + "." + std::string(key)));
        return std::optional<bool>(value);
    }
    return std::optional<bool>{};
}

ParseResult<std::optional<int64_t>> parse_optional_integer(
    toml::table const& table, std::string_view key, std::string const& context)
{
    if (auto const* node = table.get(key))
    {
        LWM_TRYV(value, expect_integer(*node, context + "." + std::string(key)));
        return std::optional<int64_t>(value);
    }
    return std::optional<int64_t>{};
}

ParseResult<std::optional<double>> parse_optional_number(
    toml::table const& table, std::string_view key, std::string const& context)
{
    if (auto const* node = table.get(key))
    {
        LWM_TRYV(value, expect_number(*node, context + "." + std::string(key)));
        return std::optional<double>(value);
    }
    return std::optional<double>{};
}

ParseVoid validate_regex_pattern(std::string const& pattern, std::string const& context)
{
    if (pattern.empty())
    {
        return std::unexpected(context + " must not be empty");
    }

    try
    {
        std::regex("^(?:" + pattern + ")$", std::regex::ECMAScript);
    }
    catch (std::regex_error const& e)
    {
        return std::unexpected(context + " has invalid regex: " + std::string(e.what()));
    }

    return {};
}

ParseVoid validate_rule_type(std::string const& type, std::string const& context)
{
    static constexpr std::array<std::string_view, 15> allowed = {
        "desktop",
        "dock",
        "toolbar",
        "menu",
        "utility",
        "splash",
        "dialog",
        "dropdown_menu",
        "dropdownmenu",
        "popup_menu",
        "popupmenu",
        "tooltip",
        "notification",
        "combo",
        "dnd",
    };

    std::string lowered = type;
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lowered == "normal" || std::ranges::find(allowed, lowered) != allowed.end())
        return {};
    return std::unexpected(context + " has unknown window type '" + type + "'");
}

ParseVoid validate_layer(std::string const& layer, std::string const& context)
{
    std::string lowered = layer;
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lowered == "overlay")
        return {};
    return std::unexpected(context + " has unknown layer '" + layer + "'");
}

template<typename T>
ParseVoid parse_regex_matchers(toml::table const& table, std::string const& context, T& target)
{
    auto parse_pattern = [&](std::string_view key, std::optional<std::string>& field) -> ParseVoid
    {
        LWM_TRYV(pattern, parse_optional_string(table, key, context));
        if (!pattern)
            return {};

        LWM_TRY(validate_regex_pattern(*pattern, context + "." + std::string(key)));
        field = std::move(*pattern);
        return {};
    };

    LWM_TRY(parse_pattern("class", target.class_pattern));
    LWM_TRY(parse_pattern("instance", target.instance_pattern));
    LWM_TRY(parse_pattern("title", target.title_pattern));
    return {};
}

template<typename T>
bool has_matcher_patterns(T const& target)
{
    return target.class_pattern.has_value() || target.instance_pattern.has_value() || target.title_pattern.has_value();
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

ParseResult<std::string> validate_modifier_string(std::string const& mod, std::string const& context)
{
    if (mod.empty())
        return std::string{};

    std::stringstream stream(mod);
    std::string token;
    std::vector<std::string> tokens;
    std::set<std::string> seen;

    while (std::getline(stream, token, '+'))
    {
        if (token.empty())
            return std::unexpected(context + " contains an empty modifier token");
        if (token == "control")
            token = "ctrl";
        if (token != "super" && token != "shift" && token != "ctrl" && token != "alt")
        {
            return std::unexpected(context + " has unknown modifier '" + token + "'");
        }
        if (!seen.insert(token).second)
        {
            return std::unexpected(context + " repeats modifier '" + token + "'");
        }
        tokens.push_back(token);
    }

    std::ostringstream normalized;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        if (i != 0)
            normalized << '+';
        normalized << tokens[i];
    }
    return normalized.str();
}

ParseResult<std::pair<std::string, std::string>> parse_key_combo(std::string const& combo, std::string const& context)
{
    if (combo.empty())
        return std::unexpected(context + " must not be empty");

    std::stringstream stream(combo);
    std::string token;
    std::vector<std::string> parts;
    while (std::getline(stream, token, '+'))
    {
        if (token.empty())
            return std::unexpected(context + " contains an empty token");
        parts.push_back(token);
    }

    if (parts.empty())
        return std::unexpected(context + " must contain a key");

    std::string key = parts.back();
    parts.pop_back();

    std::ostringstream mod;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0)
            mod << '+';
        mod << parts[i];
    }

    LWM_TRYV(normalized_mod, validate_modifier_string(mod.str(), context));
    if (KeybindManager::parse_keysym(key) == XCB_NO_SYMBOL)
    {
        return std::unexpected(context + " has unknown key '" + key + "'");
    }

    return std::make_pair(std::move(normalized_mod), std::move(key));
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
        return std::unexpected(
            context + " must be in range 0.." + std::to_string(count == 0 ? 0 : count - 1)
        );
    }
    return {};
}

ParseVoid add_binding(
    std::vector<KeybindConfig>& keybinds,
    std::set<KeyBinding>& seen,
    KeybindConfig keybind,
    std::string const& context
)
{
    KeyBinding binding { KeybindManager::parse_modifier(keybind.mod), KeybindManager::parse_keysym(keybind.key) };
    if (!seen.insert(binding).second)
    {
        return std::unexpected(context + " duplicates an existing binding for '" + keybind.key + "'");
    }
    keybinds.push_back(std::move(keybind));
    return {};
}

template<typename Predicate>
void erase_matching_bindings(
    std::vector<KeybindConfig>& keybinds,
    std::set<KeyBinding>& seen,
    Predicate&& predicate
)
{
    auto it = keybinds.begin();
    while (it != keybinds.end())
    {
        if (!predicate(*it))
        {
            ++it;
            continue;
        }

        seen.erase({ KeybindManager::parse_modifier(it->mod), KeybindManager::parse_keysym(it->key) });
        it = keybinds.erase(it);
    }
}

constexpr std::array<std::string_view, 18> BIND_ACTION_KEYS = {
    "spawn",
    "kill",
    "reload_config",
    "restart",
    "switch_workspace",
    "toggle_workspace",
    "move_to_workspace",
    "focus_monitor",
    "move_to_monitor",
    "toggle_fullscreen",
    "toggle_float",
    "focus_next",
    "focus_prev",
    "ratio_grow",
    "ratio_shrink",
    "toggle_scratchpad",
    "scratchpad_stash",
    "scratchpad_cycle",
};

ParseVoid reject_unknown_bind_keys(toml::table const& table, std::string const& context)
{
    for (auto const& [key, _] : table)
    {
        if (key == "key")
            continue;
        if (std::ranges::find(BIND_ACTION_KEYS, key.str()) != BIND_ACTION_KEYS.end())
            continue;
        return std::unexpected(context + " has unknown key '" + std::string(key.str()) + "'");
    }
    return {};
}

int count_present_bind_actions(toml::table const& table)
{
    int count = 0;
    for (std::string_view key : BIND_ACTION_KEYS)
    {
        if (table.contains(key))
            ++count;
    }
    return count;
}

ParseVoid parse_workspace_binding_action(
    toml::node const& node,
    std::string const& context,
    std::string_view action_name,
    Config const& config,
    KeybindConfig& keybind
)
{
    LWM_TRYV(workspace, expect_integer(node, context + "." + std::string(action_name)));
    LWM_TRY(validate_workspace_index(workspace, config.workspaces.count, context + "." + std::string(action_name)));
    keybind.action = std::string(action_name);
    keybind.workspace = static_cast<int>(workspace);
    return {};
}

ParseVoid parse_direction_binding_action(
    toml::node const& node,
    std::string const& context,
    std::string_view action_name,
    KeybindConfig& keybind
)
{
    LWM_TRYV(direction, expect_integer(node, context + "." + std::string(action_name)));
    if (direction != -1 && direction != 1)
        return std::unexpected(context + "." + std::string(action_name) + " must be -1 or 1");
    keybind.action = std::string(action_name);
    keybind.direction = static_cast<int>(direction);
    return {};
}

ParseVoid parse_enabled_binding_action(
    toml::node const& node,
    std::string const& context,
    std::string_view action_name,
    KeybindConfig& keybind
)
{
    LWM_TRY(expect_enabled_flag(node, context + "." + std::string(action_name)));
    keybind.action = std::string(action_name);
    return {};
}

ParseVoid parse_scratchpad_binding_action(
    toml::node const& node,
    std::string const& context,
    Config const& config,
    KeybindConfig& keybind
)
{
    LWM_TRYV(scratchpad, expect_string(node, context + ".toggle_scratchpad"));
    if (!has_scratchpad_name(config, scratchpad))
        return std::unexpected(context + ".toggle_scratchpad points to unknown scratchpad '" + scratchpad + "'");
    keybind.action = "toggle_scratchpad";
    keybind.target = std::move(scratchpad);
    return {};
}

ParseVoid parse_bind_action(toml::table const& table, std::string const& context, Config const& config, KeybindConfig& keybind)
{
    if (count_present_bind_actions(table) != 1)
        return std::unexpected(context + " must define exactly one action");

    if (auto const* node = table.get("spawn"))
    {
        keybind.action = "spawn";
        LWM_TRYV(command, parse_command_config(*node, context + ".spawn", config.commands, true));
        keybind.command = std::move(command);
        return {};
    }
    if (auto const* node = table.get("kill"))
        return parse_enabled_binding_action(*node, context, "kill", keybind);
    if (auto const* node = table.get("reload_config"))
        return parse_enabled_binding_action(*node, context, "reload_config", keybind);
    if (auto const* node = table.get("restart"))
        return parse_enabled_binding_action(*node, context, "restart", keybind);
    if (auto const* node = table.get("switch_workspace"))
        return parse_workspace_binding_action(*node, context, "switch_workspace", config, keybind);
    if (auto const* node = table.get("toggle_workspace"))
        return parse_enabled_binding_action(*node, context, "toggle_workspace", keybind);
    if (auto const* node = table.get("move_to_workspace"))
        return parse_workspace_binding_action(*node, context, "move_to_workspace", config, keybind);
    if (auto const* node = table.get("focus_monitor"))
        return parse_direction_binding_action(*node, context, "focus_monitor", keybind);
    if (auto const* node = table.get("move_to_monitor"))
        return parse_direction_binding_action(*node, context, "move_to_monitor", keybind);
    if (auto const* node = table.get("toggle_fullscreen"))
        return parse_enabled_binding_action(*node, context, "toggle_fullscreen", keybind);
    if (auto const* node = table.get("toggle_float"))
        return parse_enabled_binding_action(*node, context, "toggle_float", keybind);
    if (auto const* node = table.get("focus_next"))
        return parse_enabled_binding_action(*node, context, "focus_next", keybind);
    if (auto const* node = table.get("focus_prev"))
        return parse_enabled_binding_action(*node, context, "focus_prev", keybind);
    if (auto const* node = table.get("ratio_grow"))
        return parse_enabled_binding_action(*node, context, "ratio_grow", keybind);
    if (auto const* node = table.get("ratio_shrink"))
        return parse_enabled_binding_action(*node, context, "ratio_shrink", keybind);
    if (auto const* node = table.get("toggle_scratchpad"))
        return parse_scratchpad_binding_action(*node, context, config, keybind);
    if (auto const* node = table.get("scratchpad_stash"))
        return parse_enabled_binding_action(*node, context, "scratchpad_stash", keybind);
    if (auto const* node = table.get("scratchpad_cycle"))
        return parse_enabled_binding_action(*node, context, "scratchpad_cycle", keybind);

    return std::unexpected(context + " must define exactly one action");
}

ParseResult<RuleGeometry> parse_geometry(toml::node const& node, std::string const& context)
{
    LWM_TRYV(table, expect_table(node, context));
    LWM_TRY(reject_unknown_keys(*table, { "x", "y", "width", "height" }, context));

    RuleGeometry geometry;
    LWM_TRYV(x, parse_optional_integer(*table, "x", context));
    if (x)
        geometry.x = static_cast<int32_t>(*x);
    LWM_TRYV(y, parse_optional_integer(*table, "y", context));
    if (y)
        geometry.y = static_cast<int32_t>(*y);
    LWM_TRYV(width, parse_optional_integer(*table, "width", context));
    if (width)
    {
        if (*width <= 0)
            return std::unexpected(context + ".width must be positive");
        geometry.width = static_cast<uint32_t>(*width);
    }
    LWM_TRYV(height, parse_optional_integer(*table, "height", context));
    if (height)
    {
        if (*height <= 0)
            return std::unexpected(context + ".height must be positive");
        geometry.height = static_cast<uint32_t>(*height);
    }

    return geometry;
}

ParseVoid parse_rule_match_table(toml::table const& table, std::string const& context, WindowRuleConfig& rule)
{
    LWM_TRY(reject_unknown_keys(table, { "class", "instance", "title", "type", "transient" }, context));
    LWM_TRY(parse_regex_matchers(table, context, rule));
    LWM_TRYV(type, parse_optional_string(table, "type", context));
    if (type)
    {
        LWM_TRY(validate_rule_type(*type, context + ".type"));
        rule.type = std::move(*type);
    }
    LWM_TRYV(transient, parse_optional_bool(table, "transient", context));
    if (transient)
        rule.transient = *transient;

    return {};
}

ParseVoid parse_scratchpad_match_table(toml::table const& table, std::string const& context, ScratchpadConfig& scratchpad)
{
    LWM_TRY(reject_unknown_keys(table, { "class", "instance", "title" }, context));

    LWM_TRY(parse_regex_matchers(table, context, scratchpad));
    if (!has_matcher_patterns(scratchpad))
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

    LWM_TRY(reject_unknown_keys(table,
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
            "layer",
            "borderless",
            "geometry",
            "center",
            "scratchpad",
        },
        context));

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
        if (*monitor < 0)
            return std::unexpected(context + ".monitor must be non-negative");
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
    LWM_TRYV(layer, parse_optional_string(table, "layer", context));
    if (layer)
    {
        LWM_TRY(validate_layer(*layer, context + ".layer"));
        rule.layer = std::move(*layer);
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

std::vector<KeybindConfig> build_default_keybinds(Config const& config)
{
    std::vector<KeybindConfig> keybinds;

    auto add_spawn = [&](std::string mod, std::string key, std::string_view command_name)
    {
        auto it = config.commands.find(std::string(command_name));
        if (it == config.commands.end())
            return;

        KeybindConfig keybind;
        keybind.mod = std::move(mod);
        keybind.key = std::move(key);
        keybind.action = "spawn";
        keybind.command = it->second;
        keybinds.push_back(std::move(keybind));
    };

    auto add_workspace_binds = [&](std::string const& mod, std::vector<std::string> const& keys, std::string_view action)
    {
        size_t limit = std::min(config.workspaces.count, keys.size());
        for (size_t i = 0; i < limit; ++i)
        {
            KeybindConfig keybind;
            keybind.mod = mod;
            keybind.key = keys[i];
            keybind.action = std::string(action);
            keybind.workspace = static_cast<int>(i);
            keybinds.push_back(std::move(keybind));
        }
    };

    add_spawn("super", "Return", "terminal");
    add_spawn("super", "d", "launcher");

    keybinds.push_back({ .mod = "super", .key = "q", .action = "kill" });
    add_workspace_binds(
        "super",
        { "ampersand", "eacute", "quotedbl", "apostrophe", "parenleft", "minus", "egrave", "underscore", "ccedilla", "agrave" },
        "switch_workspace"
    );
    add_workspace_binds("super", { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" }, "switch_workspace");
    add_workspace_binds(
        "super+shift",
        { "ampersand", "eacute", "quotedbl", "apostrophe", "parenleft", "minus", "egrave", "underscore", "ccedilla", "agrave" },
        "move_to_workspace"
    );
    add_workspace_binds("super+shift", { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" }, "move_to_workspace");

    keybinds.push_back({ .mod = "super", .key = "Left", .action = "focus_monitor", .direction = -1 });
    keybinds.push_back({ .mod = "super", .key = "Right", .action = "focus_monitor", .direction = 1 });
    keybinds.push_back({ .mod = "super+shift", .key = "Left", .action = "move_to_monitor", .direction = -1 });
    keybinds.push_back({ .mod = "super+shift", .key = "Right", .action = "move_to_monitor", .direction = 1 });
    keybinds.push_back({ .mod = "super", .key = "f", .action = "toggle_fullscreen" });
    keybinds.push_back({ .mod = "super+shift", .key = "f", .action = "toggle_float" });
    keybinds.push_back({ .mod = "super", .key = "j", .action = "focus_next" });
    keybinds.push_back({ .mod = "super", .key = "k", .action = "focus_prev" });
    keybinds.push_back({ .mod = "super", .key = "h", .action = "ratio_shrink" });
    keybinds.push_back({ .mod = "super", .key = "l", .action = "ratio_grow" });

    return keybinds;
}

} // namespace

Config default_config()
{
    Config cfg;
    cfg.workspaces.names = default_workspace_names(cfg.workspaces.count);

    cfg.commands["terminal"] = CommandConfig::argv_command({ "/usr/local/bin/st" });
    cfg.commands["browser"] = CommandConfig::argv_command({ "/usr/bin/firefox" });
    cfg.commands["launcher"] = CommandConfig::argv_command({ "dmenu_run" });
    cfg.keybinds = build_default_keybinds(cfg);

    cfg.mousebinds = {
        { "super", 1, "drag_window" },
        { "super", 3, "resize_floating" },
        { "super", 2, "toggle_float" },
    };

    return cfg;
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

        Config cfg = default_config();
        bool has_bind_overrides = root.contains("binds");
        bool has_workspace_bind_overrides = root.contains("workspace_binds");

        if (auto appearance = root["appearance"].as_table())
        {
            LWM_TRY(reject_unknown_keys(*appearance, { "padding", "border_width", "border_color", "urgent_border_color" }, "[appearance]"));

            LWM_TRYV(padding, parse_optional_integer(*appearance, "padding", "[appearance]"));
            if (padding)
            {
                if (*padding < 0)
                    return std::unexpected("[appearance].padding must be non-negative");
                cfg.appearance.padding = static_cast<uint32_t>(*padding);
            }
            LWM_TRYV(border_width, parse_optional_integer(*appearance, "border_width", "[appearance]"));
            if (border_width)
            {
                if (*border_width < 0)
                    return std::unexpected("[appearance].border_width must be non-negative");
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
            LWM_TRY(reject_unknown_keys(*layout, { "strategy", "default_ratio", "min_ratio", "resize_grab_threshold" }, "[layout]"));

            LWM_TRYV(strategy, parse_optional_string(*layout, "strategy", "[layout]"));
            if (strategy)
                cfg.layout.strategy = std::move(*strategy);
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
                LWM_TRYV(command, parse_command_config(node, "[commands]." + std::string(name.str()), cfg.commands, false));
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
                if (*workspace_count < 1)
                    return std::unexpected("[workspaces].count must be >= 1");
                cfg.workspaces.count = static_cast<size_t>(*workspace_count);
                workspaces_count_set = true;
            }
            if (auto const* names = workspaces->get("names"))
            {
                LWM_TRYV(workspace_names, parse_string_array(*names, "[workspaces].names"));
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
                    LWM_TRYV(command, parse_command_config(*item, "[autostart].commands[" + std::to_string(i) + "]", cfg.commands, true));
                    cfg.autostart.commands.push_back(std::move(command));
                }
            }
        }

        if (has_bind_overrides)
            cfg.keybinds.clear();

        std::set<KeyBinding> seen_bindings;
        for (auto const& keybind : cfg.keybinds)
        {
            seen_bindings.insert({ KeybindManager::parse_modifier(keybind.mod), KeybindManager::parse_keysym(keybind.key) });
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
                LWM_TRY(reject_unknown_bind_keys(*table, context));

                auto const* key_node = table->get("key");
                if (!key_node)
                    return std::unexpected(context + ".key is required");
                LWM_TRYV(key_combo, expect_string(*key_node, context + ".key"));
                LWM_TRYV(combo, parse_key_combo(key_combo, context + ".key"));

                KeybindConfig keybind;
                keybind.mod = std::move(combo.first);
                keybind.key = std::move(combo.second);
                LWM_TRY(parse_bind_action(*table, context, cfg, keybind));
                LWM_TRY(add_binding(cfg.keybinds, seen_bindings, std::move(keybind), context));
            }
        }

        std::set<std::pair<std::string, std::string>> replaced_workspace_groups;
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
                LWM_TRYV(mod, validate_modifier_string(mod_value, context + ".mod"));
                LWM_TRYV(keys, parse_string_array(*keys_node, context + ".keys"));
                if (keys.size() != cfg.workspaces.count)
                {
                    return std::unexpected(
                        context + ".keys must contain exactly " + std::to_string(cfg.workspaces.count) + " entries"
                    );
                }

                std::string action = mode == "switch" ? "switch_workspace" : "move_to_workspace";
                if (has_workspace_bind_overrides && !has_bind_overrides)
                {
                    auto const [_, inserted] = replaced_workspace_groups.insert({ mod, action });
                    if (inserted)
                    {
                        erase_matching_bindings(
                            cfg.keybinds,
                            seen_bindings,
                            [&](KeybindConfig const& existing)
                            { return existing.mod == mod && existing.action == action; }
                        );
                    }
                }

                for (size_t workspace = 0; workspace < keys.size(); ++workspace)
                {
                    if (KeybindManager::parse_keysym(keys[workspace]) == XCB_NO_SYMBOL)
                    {
                        return std::unexpected(
                            context + ".keys[" + std::to_string(workspace) + "] has unknown key '" + keys[workspace] + "'"
                        );
                    }

                    KeybindConfig keybind;
                    keybind.mod = mod;
                    keybind.key = keys[workspace];
                    keybind.action = action;
                    keybind.workspace = static_cast<int>(workspace);
                    LWM_TRY(add_binding(
                        cfg.keybinds,
                        seen_bindings,
                        std::move(keybind),
                        context + ".keys[" + std::to_string(workspace) + "]"
                    ));
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
                    LWM_TRYV(mod, validate_modifier_string(mod_value, context + ".mod"));
                    mousebind.mod = std::move(mod);
                }
                if (auto const* button_node = table->get("button"))
                {
                    LWM_TRYV(button, expect_integer(*button_node, context + ".button"));
                    if (button <= 0 || button > 255)
                        return std::unexpected(context + ".button must be in range 1..255");
                    mousebind.button = static_cast<int>(button);
                }
                else
                {
                    return std::unexpected(context + ".button is required");
                }

                if (auto const* action_node = table->get("action"))
                {
                    LWM_TRYV(action, expect_string(*action_node, context + ".action"));
                    mousebind.action = std::move(action);
                }
                else
                {
                    return std::unexpected(context + ".action is required");
                }

                if (mousebind.action != "drag_window" && mousebind.action != "resize_floating" && mousebind.action != "toggle_float")
                    return std::unexpected(context + ".action has unknown mouse action '" + mousebind.action + "'");

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

std::optional<Config> load_config(std::string const& path)
{
    auto result = load_config_result(path);
    if (!result)
        return std::nullopt;
    return *result;
}

#undef LWM_TRY
#undef LWM_TRYV

} // namespace lwm
