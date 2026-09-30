#include "config.hpp"
#include <X11/Xlib.h>
// Xlib defines stacking-mode macros that collide with LayerHint enumerators.
#undef Above
#undef Below
#include <algorithm>
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
    do                                                       \
    {                                                        \
        auto lwm_try_result_ = (expr);                       \
        if (!lwm_try_result_)                                \
            return std::unexpected(lwm_try_result_.error()); \
    } while (false)

#define LWM_TRYV(name, expr)                           \
    auto name##_result = (expr);                       \
    if (!name##_result)                                \
        return std::unexpected(name##_result.error()); \
    auto name = std::move(*name##_result)

// ---------------------------------------------------------------------------
// Typed node access. Every error names the full key path.
// ---------------------------------------------------------------------------

ParseVoid reject_unknown_keys(
    toml::table const& table,
    std::initializer_list<std::string_view> allowed,
    std::string const& context
)
{
    for (auto const& [key, _] : table)
        if (std::ranges::find(allowed, key.str()) == allowed.end())
            return std::unexpected(context + " has unknown key '" + std::string(key.str()) + "'");
    return { };
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
        LWM_TRYV(value, expect_string(*array->get(i), context + "[" + std::to_string(i) + "]"));
        values.push_back(std::move(value));
    }
    return values;
}

// Optional field readers: absent keys yield nullopt, present keys must be valid.
template <typename T, typename Expect>
ParseResult<std::optional<T>>
optional_field(toml::table const& table, std::string_view key, std::string const& context, Expect expect)
{
    auto const* node = table.get(key);
    if (!node)
        return std::optional<T>{ };
    LWM_TRYV(value, expect(*node, context + "." + std::string(key)));
    return std::optional<T>(std::move(value));
}

auto optional_string(toml::table const& table, std::string_view key, std::string const& context)
{
    return optional_field<std::string>(table, key, context, expect_string);
}

auto optional_bool(toml::table const& table, std::string_view key, std::string const& context)
{
    return optional_field<bool>(table, key, context, expect_bool);
}

auto optional_number(toml::table const& table, std::string_view key, std::string const& context)
{
    return optional_field<double>(table, key, context, expect_number);
}

// One range check for every bounded integer setting.
ParseResult<std::optional<int64_t>> optional_integer(
    toml::table const& table,
    std::string_view key,
    std::string const& context,
    int64_t min,
    int64_t max,
    std::string_view requirement = { }
)
{
    LWM_TRYV(value, (optional_field<int64_t>(table, key, context, expect_integer)));
    if (value && (*value < min || *value > max))
    {
        std::string field = context + "." + std::string(key);
        if (!requirement.empty())
            return std::unexpected(field + " must " + std::string(requirement));
        return std::unexpected(field + " must be in range " + std::to_string(min) + ".." + std::to_string(max));
    }
    return value;
}

// Iterate an optional array of tables, naming each entry "<label>#<index>".
template <typename Handler>
ParseVoid for_each_table(toml::table const& root, std::string_view key, std::string_view label, Handler handler)
{
    auto const* array = root.get_as<toml::array>(key);
    if (!array)
        return { };
    for (size_t i = 0; i < array->size(); ++i)
    {
        std::string context = std::string(label) + "#" + std::to_string(i);
        LWM_TRYV(table, expect_table(*array->get(i), context));
        LWM_TRY(handler(*table, context));
    }
    return { };
}

// ---------------------------------------------------------------------------
// Shared value grammars
// ---------------------------------------------------------------------------

ParseResult<WindowType> parse_window_type(std::string type, std::string const& context)
{
    std::ranges::transform(type, type.begin(), [](unsigned char c) { return std::tolower(c); });
    static constexpr std::pair<std::string_view, WindowType> types[] = {
        { "desktop", WindowType::Desktop },
        { "dock", WindowType::Dock },
        { "toolbar", WindowType::Toolbar },
        { "menu", WindowType::Menu },
        { "utility", WindowType::Utility },
        { "splash", WindowType::Splash },
        { "dialog", WindowType::Dialog },
        { "dropdown_menu", WindowType::DropdownMenu },
        { "dropdownmenu", WindowType::DropdownMenu },
        { "popup_menu", WindowType::PopupMenu },
        { "popupmenu", WindowType::PopupMenu },
        { "tooltip", WindowType::Tooltip },
        { "notification", WindowType::Notification },
        { "combo", WindowType::Combo },
        { "dnd", WindowType::Dnd },
        { "normal", WindowType::Normal },
    };
    for (auto const& [name, value] : types)
        if (name == type)
            return value;
    return std::unexpected(context + " has unknown window type '" + type + "'");
}

ParseVoid parse_matchers(toml::table const& table, std::string const& context, WindowMatcher& match)
{
    auto pattern = [&](std::string_view key, std::optional<std::regex>& field) -> ParseVoid
    {
        LWM_TRYV(text, optional_string(table, key, context));
        if (!text)
            return { };
        if (text->empty())
            return std::unexpected(context + "." + std::string(key) + " must not be empty");
        try
        {
            field.emplace("^(?:" + *text + ")$", std::regex::ECMAScript | std::regex::optimize);
        }
        catch (std::regex_error const& error)
        {
            return std::unexpected(context + "." + std::string(key) + " has invalid regex: " + error.what());
        }
        return { };
    };
    LWM_TRY(pattern("class", match.class_regex));
    LWM_TRY(pattern("instance", match.instance_regex));
    LWM_TRY(pattern("title", match.title_regex));
    return { };
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
    // A leading '+' is an empty modifier, not an unmodified key.
    if (separator == 0)
        return std::unexpected(context + " contains an empty modifier");
    LWM_TRYV(
        modifier,
        parse_modifiers(separator == combo.npos ? std::string_view{ } : std::string_view(combo).substr(0, separator), context)
    );
    LWM_TRYV(keysym, parse_keysym(combo.substr(separator == combo.npos ? 0 : separator + 1), context));
    return KeyBinding{ modifier, keysym };
}

ParseResult<CommandConfig> parse_command(
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
    if (table->size() != 1)
        return std::unexpected(context + " must contain exactly one of 'ref', 'shell', or 'argv'");
    if (auto const* ref_node = table->get("ref"))
    {
        LWM_TRYV(ref, expect_string(*ref_node, context + ".ref"));
        auto it = registry.find(ref);
        if (it == registry.end())
            return std::unexpected(context + ".ref points to unknown command '" + ref + "'");
        return it->second;
    }
    if (auto const* shell_node = table->get("shell"))
    {
        LWM_TRYV(shell, expect_string(*shell_node, context + ".shell"));
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

ParseResult<size_t> parse_workspace_index(toml::node const& node, std::string const& context, size_t count)
{
    LWM_TRYV(index, expect_integer(node, context));
    if (index < 0 || static_cast<size_t>(index) >= count)
        return std::unexpected(context + " must be in range 0.." + std::to_string(count - 1));
    return static_cast<size_t>(index);
}

ParseResult<std::string> parse_scratchpad_name(toml::node const& node, std::string const& context, Config const& config)
{
    LWM_TRYV(name, expect_string(node, context));
    if (std::ranges::none_of(config.scratchpads, [&](auto const& scratchpad) { return scratchpad.name == name; }))
        return std::unexpected(context + " points to unknown scratchpad '" + name + "'");
    return name;
}

ParseResult<double> parse_ratio(toml::node const& node, std::string const& context, LayoutConfig const& layout)
{
    LWM_TRYV(value, expect_number(node, context));
    if (!layout.accepts_ratio(value))
        return std::unexpected(context + " must respect min_ratio bounds");
    return value;
}

// ---------------------------------------------------------------------------
// Key-binding actions: exactly one action key per [[binds]] entry.
// ---------------------------------------------------------------------------

ParseResult<Action> parse_bind_action(
    std::string_view name,
    toml::node const& value,
    std::string const& context,
    Config const& config
)
{
    std::string const field = context + "." + std::string(name);
    using namespace action;
    // Actions without a value are enabled with `name = true`; names come from action_name().
    static Action const flags[] = {
        Kill{ },         ReloadConfig{ },       Restart{ },         ToggleFullscreen{ }, ToggleFloat{ },
        FocusCycle{ true }, FocusCycle{ false }, ToggleWorkspace{ }, CycleWorkspace{ 1 }, CycleWorkspace{ -1 },
        SwapTile{ 1 },    SwapTile{ -1 },        ResetRatios{ },     ScratchpadStash{ },  ScratchpadCycle{ },
    };
    for (auto const& action : flags)
    {
        if (action_name(action) != name)
            continue;
        LWM_TRYV(enabled, expect_bool(value, field));
        if (!enabled)
            return std::unexpected(field + " must be true when present");
        return action;
    }
    if (name == "spawn")
    {
        LWM_TRYV(command, parse_command(value, field, config.commands, true));
        return Spawn{ std::move(command) };
    }
    if (name == "exec")
    {
        LWM_TRYV(binary, expect_string(value, field));
        if (binary.empty())
            return std::unexpected(field + " must not be empty");
        return Exec{ std::move(binary) };
    }
    if (name == "switch_workspace" || name == "move_to_workspace")
    {
        LWM_TRYV(workspace, parse_workspace_index(value, field, config.workspaces.count));
        return name == "switch_workspace" ? Action{ SwitchWorkspace{ workspace } } : Action{ MoveToWorkspace{ workspace } };
    }
    if (name == "focus_monitor" || name == "move_to_monitor")
    {
        LWM_TRYV(direction, expect_integer(value, field));
        if (direction != -1 && direction != 1)
            return std::unexpected(field + " must be -1 or 1");
        int step = static_cast<int>(direction);
        return name == "focus_monitor" ? Action{ FocusMonitor{ step } } : Action{ MoveToMonitor{ step } };
    }
    if (name == "toggle_scratchpad" || name == "cancel_scratchpad_launch")
    {
        LWM_TRYV(scratchpad, parse_scratchpad_name(value, field, config));
        return name == "toggle_scratchpad" ? Action{ ScratchpadToggle{ std::move(scratchpad) } }
                                           : Action{ ScratchpadCancelLaunch{ std::move(scratchpad) } };
    }
    if (name == "set_layout")
    {
        LWM_TRYV(text, expect_string(value, field));
        auto strategy = parse_layout_strategy(text);
        if (!strategy)
            return std::unexpected(field + " has unknown value '" + text + "' (expected 'master-stack' or 'monocle')");
        return SetLayout{ *strategy };
    }
    if (name == "set_ratio")
    {
        LWM_TRYV(ratio, parse_ratio(value, field, config.layout));
        return SetRatio{ ratio };
    }
    if (name == "adjust_ratio")
    {
        LWM_TRYV(delta, expect_number(value, field));
        return AdjustRatio{ delta };
    }
    return std::unexpected(context + " has unknown key '" + std::string(name) + "'");
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

ParseVoid parse_appearance(toml::table const& table, AppearanceConfig& appearance)
{
    std::string const context = "[appearance]";
    LWM_TRY(reject_unknown_keys(table, { "padding", "border_width", "border_color", "urgent_border_color" }, context));
    LWM_TRYV(padding, optional_integer(table, "padding", context, 0, 65535));
    LWM_TRYV(border_width, optional_integer(table, "border_width", context, 0, 65535));
    LWM_TRYV(border_color, optional_integer(table, "border_color", context, 0, 0xFFFFFFFF, "be in range 0x000000..0xFFFFFFFF"));
    LWM_TRYV(
        urgent_color,
        optional_integer(table, "urgent_border_color", context, 0, 0xFFFFFFFF, "be in range 0x000000..0xFFFFFFFF")
    );
    appearance.padding = static_cast<uint32_t>(padding.value_or(appearance.padding));
    appearance.border_width = static_cast<uint32_t>(border_width.value_or(appearance.border_width));
    appearance.border_color = static_cast<uint32_t>(border_color.value_or(appearance.border_color));
    appearance.urgent_border_color = static_cast<uint32_t>(urgent_color.value_or(appearance.urgent_border_color));
    return { };
}

ParseVoid parse_layout(toml::table const& table, LayoutConfig& layout)
{
    std::string const context = "[layout]";
    LWM_TRY(reject_unknown_keys(table, { "strategy", "default_ratio", "min_ratio", "resize_grab_threshold" }, context));
    LWM_TRYV(strategy, optional_string(table, "strategy", context));
    if (strategy)
    {
        auto parsed = parse_layout_strategy(*strategy);
        if (!parsed)
            return std::unexpected(
                "[layout].strategy has unknown value '" + *strategy + "' (expected 'master-stack' or 'monocle')"
            );
        layout.strategy = *parsed;
    }
    LWM_TRYV(min_ratio, optional_number(table, "min_ratio", context));
    if (min_ratio)
    {
        if (*min_ratio < 0.05 || *min_ratio > 0.45)
            return std::unexpected("[layout].min_ratio must be in range 0.05..0.45");
        layout.min_ratio = *min_ratio;
    }
    if (auto const* node = table.get("default_ratio"))
    {
        LWM_TRYV(ratio, parse_ratio(*node, "[layout].default_ratio", layout));
        layout.default_ratio = ratio;
    }
    LWM_TRYV(threshold, optional_integer(table, "resize_grab_threshold", context, 1, 100));
    layout.resize_grab_threshold = static_cast<uint32_t>(threshold.value_or(layout.resize_grab_threshold));
    return { };
}

ParseVoid parse_focus(toml::table const& table, FocusConfig& focus)
{
    LWM_TRY(reject_unknown_keys(table, { "warp_cursor_on_monitor_change" }, "[focus]"));
    LWM_TRYV(warp, optional_bool(table, "warp_cursor_on_monitor_change", "[focus]"));
    focus.warp_cursor_on_monitor_change = warp.value_or(focus.warp_cursor_on_monitor_change);
    return { };
}

ParseVoid parse_commands(toml::table const& table, std::map<std::string, CommandConfig>& commands)
{
    for (auto const& [name, node] : table)
    {
        std::string context = "[commands]." + std::string(name.str());
        LWM_TRYV(command, parse_command(node, context, commands, false));
        commands[std::string(name.str())] = std::move(command);
    }
    return { };
}

// When count is omitted, nonempty names determine it; missing names become
// numeric labels and excess names are discarded.
ParseVoid parse_workspaces(toml::table const& table, WorkspacesConfig& workspaces)
{
    std::string const context = "[workspaces]";
    LWM_TRY(reject_unknown_keys(table, { "count", "names" }, context));
    LWM_TRYV(count, optional_integer(table, "count", context, 1, 65535));
    std::vector<std::string> names;
    if (auto const* node = table.get("names"))
    {
        LWM_TRYV(parsed, parse_string_array(*node, "[workspaces].names"));
        if (parsed.size() > 65535)
            return std::unexpected("[workspaces].names cannot contain more than 65535 entries");
        names = std::move(parsed);
    }
    workspaces.count = count ? static_cast<size_t>(*count) : names.empty() ? workspaces.count : names.size();
    size_t named = std::min(names.size(), workspaces.count);
    names.resize(workspaces.count);
    for (size_t i = named; i < names.size(); ++i) names[i] = std::to_string(i + 1);
    workspaces.names = std::move(names);
    return { };
}

ParseVoid parse_scratchpad(toml::table const& table, std::string const& context, Config& config)
{
    LWM_TRY(reject_unknown_keys(table, { "name", "spawn", "match", "size" }, context));
    ScratchpadConfig scratchpad;
    LWM_TRYV(name, optional_string(table, "name", context));
    scratchpad.name = name.value_or("");
    if (scratchpad.name.empty())
        return std::unexpected(context + ".name is required");
    if (std::ranges::any_of(config.scratchpads, [&](auto const& other) { return other.name == scratchpad.name; }))
        return std::unexpected(context + ".name duplicates scratchpad '" + scratchpad.name + "'");

    auto const* spawn = table.get("spawn");
    if (!spawn)
        return std::unexpected(context + ".spawn is required");
    LWM_TRYV(command, parse_command(*spawn, context + ".spawn", config.commands, true));
    scratchpad.spawn = std::move(command);

    auto const* match = table.get("match");
    if (!match)
        return std::unexpected(context + ".match is required");
    LWM_TRYV(match_table, expect_table(*match, context + ".match"));
    LWM_TRY(reject_unknown_keys(*match_table, { "class", "instance", "title" }, context + ".match"));
    LWM_TRY(parse_matchers(*match_table, context + ".match", scratchpad.match));
    if (scratchpad.match.empty())
        return std::unexpected(context + ".match must define at least one matcher");

    if (auto const* size = table.get("size"))
    {
        std::string size_context = context + ".size";
        LWM_TRYV(size_table, expect_table(*size, size_context));
        LWM_TRY(reject_unknown_keys(*size_table, { "width", "height" }, size_context));
        for (auto [key, field] : { std::pair{ "width", &scratchpad.width }, std::pair{ "height", &scratchpad.height } })
        {
            LWM_TRYV(fraction, optional_number(*size_table, key, size_context));
            if (fraction && (*fraction < 0.1 || *fraction > 1.0))
                return std::unexpected(size_context + "." + key + " must be in range 0.1..1.0");
            *field = fraction.value_or(*field);
        }
    }
    config.scratchpads.push_back(std::move(scratchpad));
    return { };
}

ParseVoid parse_autostart(toml::table const& table, Config& config)
{
    LWM_TRY(reject_unknown_keys(table, { "commands" }, "[autostart]"));
    config.autostart.commands.clear();
    auto const* commands = table.get("commands");
    if (!commands)
        return { };
    LWM_TRYV(list, expect_array(*commands, "[autostart].commands"));
    for (size_t i = 0; i < list->size(); ++i)
    {
        LWM_TRYV(
            command,
            parse_command(*list->get(i), "[autostart].commands[" + std::to_string(i) + "]", config.commands, true)
        );
        config.autostart.commands.push_back(std::move(command));
    }
    return { };
}

ParseVoid add_binding(Config& config, KeyBinding binding, Action action, std::string const& context)
{
    if (!config.keybinds.emplace(binding, std::move(action)).second)
        return std::unexpected(context + " duplicates an existing binding");
    return { };
}

ParseVoid parse_bind(toml::table const& table, std::string const& context, Config& config)
{
    auto const* key_node = table.get("key");
    if (!key_node)
        return std::unexpected(context + ".key is required");
    LWM_TRYV(combo_text, expect_string(*key_node, context + ".key"));
    LWM_TRYV(combo, parse_key_combo(combo_text, context + ".key"));
    if (table.size() != 2)
        return std::unexpected(context + " must define exactly one action");
    for (auto const& [key, node] : table)
    {
        if (key == "key")
            continue;
        LWM_TRYV(action, parse_bind_action(key.str(), node, context, config));
        LWM_TRY(add_binding(config, combo, std::move(action), context));
    }
    return { };
}

bool is_workspace_group(Action const& action, bool move)
{
    return move ? std::holds_alternative<action::MoveToWorkspace>(action)
                : std::holds_alternative<action::SwitchWorkspace>(action);
}

// A group replaces the default group with the same modifiers unless [[binds]]
// already replaced every default binding.
ParseVoid parse_workspace_bind(
    toml::table const& table,
    std::string const& context,
    Config& config,
    bool defaults_retained,
    std::set<std::pair<uint16_t, bool>>& replaced
)
{
    LWM_TRY(reject_unknown_keys(table, { "mode", "mod", "keys" }, context));
    auto const* mode_node = table.get("mode");
    auto const* mod_node = table.get("mod");
    auto const* keys_node = table.get("keys");
    if (!mode_node || !mod_node || !keys_node)
        return std::unexpected(context + " requires 'mode', 'mod', and 'keys'");
    LWM_TRYV(mode, expect_string(*mode_node, context + ".mode"));
    if (mode != "switch" && mode != "move")
        return std::unexpected(context + ".mode must be 'switch' or 'move'");
    bool move = mode == "move";
    LWM_TRYV(mod_text, expect_string(*mod_node, context + ".mod"));
    LWM_TRYV(mod, parse_modifiers(mod_text, context + ".mod"));
    LWM_TRYV(keys, parse_string_array(*keys_node, context + ".keys"));
    if (keys.size() != config.workspaces.count)
        return std::unexpected(
            context + ".keys must contain exactly " + std::to_string(config.workspaces.count) + " entries"
        );
    if (defaults_retained && replaced.insert({ mod, move }).second)
        std::erase_if(
            config.keybinds,
            [&](auto const& existing)
            { return existing.first.modifier == mod && is_workspace_group(existing.second, move); }
        );
    for (size_t workspace = 0; workspace < keys.size(); ++workspace)
    {
        auto key_context = context + ".keys[" + std::to_string(workspace) + "]";
        LWM_TRYV(keysym, parse_keysym(keys[workspace], key_context));
        Action action = move ? Action{ action::MoveToWorkspace{ workspace } } : Action{ action::SwitchWorkspace{ workspace } };
        LWM_TRY(add_binding(config, { mod, keysym }, std::move(action), key_context));
    }
    return { };
}

ParseVoid parse_mousebind(toml::table const& table, std::string const& context, Config& config)
{
    LWM_TRY(reject_unknown_keys(table, { "mod", "button", "action" }, context));
    MousebindConfig mousebind;
    LWM_TRYV(mod_text, optional_string(table, "mod", context));
    if (mod_text)
    {
        LWM_TRYV(mod, parse_modifiers(*mod_text, context + ".mod"));
        mousebind.modifier = mod;
    }
    LWM_TRYV(button, optional_integer(table, "button", context, 1, 255));
    if (!button)
        return std::unexpected(context + ".button is required");
    mousebind.button = static_cast<uint8_t>(*button);
    LWM_TRYV(action, optional_string(table, "action", context));
    if (!action)
        return std::unexpected(context + ".action is required");
    if (*action == "drag_window")
        mousebind.action = MouseAction::DragWindow;
    else if (*action == "resize_floating")
        mousebind.action = MouseAction::ResizeFloating;
    else if (*action == "toggle_float")
        mousebind.action = MouseAction::ToggleFloat;
    else
        return std::unexpected(context + ".action has unknown mouse action '" + *action + "'");
    config.mousebinds.push_back(mousebind);
    return { };
}

ParseResult<Geometry> parse_rule_geometry(toml::node const& node, std::string const& context)
{
    LWM_TRYV(table, expect_table(node, context));
    LWM_TRY(reject_unknown_keys(*table, { "x", "y", "width", "height" }, context));
    LWM_TRYV(x, optional_integer(*table, "x", context, -32768, 32767, "be between -32768 and 32767"));
    LWM_TRYV(y, optional_integer(*table, "y", context, -32768, 32767, "be between -32768 and 32767"));
    LWM_TRYV(width, optional_integer(*table, "width", context, 1, 65535, "be between 1 and 65535"));
    LWM_TRYV(height, optional_integer(*table, "height", context, 1, 65535, "be between 1 and 65535"));
    return Geometry{ static_cast<int16_t>(x.value_or(0)),
                     static_cast<int16_t>(y.value_or(0)),
                     static_cast<uint16_t>(width.value_or(800)),
                     static_cast<uint16_t>(height.value_or(600)) };
}

ParseVoid parse_rule_actions(toml::table const& table, std::string const& context, RuleActions& actions, Config const& config)
{
    LWM_TRY(reject_unknown_keys(
        table,
        { "floating",
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
          "scratchpad" },
        context
    ));
    if (table.empty())
        return std::unexpected(context + " must define at least one action");
    if (table.contains("workspace") && table.contains("workspace_name"))
        return std::unexpected(context + " cannot define both 'workspace' and 'workspace_name'");
    if (table.contains("monitor") && table.contains("monitor_name"))
        return std::unexpected(context + " cannot define both 'monitor' and 'monitor_name'");

    LWM_TRYV(floating, optional_bool(table, "floating", context));
    actions.floating = floating;
    if (auto const* node = table.get("workspace"))
    {
        LWM_TRYV(workspace, parse_workspace_index(*node, context + ".workspace", config.workspaces.count));
        actions.workspace = workspace;
    }
    LWM_TRYV(workspace_name, optional_string(table, "workspace_name", context));
    if (workspace_name)
    {
        auto it = std::ranges::find(config.workspaces.names, *workspace_name);
        if (it == config.workspaces.names.end())
            return std::unexpected(context + ".workspace_name points to unknown workspace '" + *workspace_name + "'");
        actions.workspace = static_cast<size_t>(it - config.workspaces.names.begin());
    }
    LWM_TRYV(monitor, optional_integer(table, "monitor", context, 0, std::numeric_limits<int>::max(), "fit a non-negative int"));
    if (monitor)
        actions.monitor = static_cast<size_t>(*monitor);
    LWM_TRYV(monitor_name, optional_string(table, "monitor_name", context));
    if (monitor_name)
        actions.monitor = std::move(*monitor_name);
    LWM_TRYV(fullscreen, optional_bool(table, "fullscreen", context));
    actions.fullscreen = fullscreen;
    LWM_TRYV(above, optional_bool(table, "above", context));
    LWM_TRYV(below, optional_bool(table, "below", context));
    if (above.value_or(false) && below.value_or(false))
        return std::unexpected(context + " cannot set both 'above' and 'below' to true");
    if (above || below)
        actions.layer = above.value_or(false) ? LayerHint::Above
            : below.value_or(false)          ? LayerHint::Below
                                             : LayerHint::Normal;
    for (auto [key, field] : { std::pair{ "sticky", &actions.sticky },
                               std::pair{ "skip_taskbar", &actions.skip_taskbar },
                               std::pair{ "skip_pager", &actions.skip_pager },
                               std::pair{ "borderless", &actions.borderless } })
    {
        LWM_TRYV(value, optional_bool(table, key, context));
        *field = value;
    }
    if (auto const* node = table.get("geometry"))
    {
        LWM_TRYV(geometry, parse_rule_geometry(*node, context + ".geometry"));
        actions.geometry = geometry;
    }
    LWM_TRYV(center, optional_bool(table, "center", context));
    actions.center = center.value_or(false);
    if (auto const* node = table.get("scratchpad"))
    {
        LWM_TRYV(scratchpad, parse_scratchpad_name(*node, context + ".scratchpad", config));
        actions.scratchpad = std::move(scratchpad);
    }
    return { };
}

ParseVoid parse_rule(toml::table const& table, std::string const& context, Config& config)
{
    LWM_TRY(reject_unknown_keys(table, { "match", "apply" }, context));
    WindowRuleConfig rule;
    if (auto const* match = table.get("match"))
    {
        std::string match_context = context + ".match";
        LWM_TRYV(match_table, expect_table(*match, match_context));
        LWM_TRY(reject_unknown_keys(*match_table, { "class", "instance", "title", "type", "transient" }, match_context));
        LWM_TRY(parse_matchers(*match_table, match_context, rule.match));
        LWM_TRYV(type, optional_string(*match_table, "type", match_context));
        if (type)
        {
            LWM_TRYV(parsed, parse_window_type(*type, match_context + ".type"));
            rule.type = parsed;
        }
        LWM_TRYV(transient, optional_bool(*match_table, "transient", match_context));
        rule.transient = transient;
    }
    auto const* apply = table.get("apply");
    if (!apply)
        return std::unexpected(context + ".apply is required");
    LWM_TRYV(apply_table, expect_table(*apply, context + ".apply"));
    LWM_TRY(parse_rule_actions(*apply_table, context + ".apply", rule.actions, config));
    config.rules.push_back(std::move(rule));
    return { };
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

void add_default_keybinds(Config& config)
{
    using namespace action;
    auto key = [](char const* name) { return static_cast<xcb_keysym_t>(XStringToKeysym(name)); };
    auto bind = [&](uint16_t mod, char const* name, Action action) { config.keybinds[{ mod, key(name) }] = std::move(action); };
    uint16_t const super = XCB_MOD_MASK_4;
    uint16_t const super_shift = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT;

    for (auto [name, command] : { std::pair{ "Return", "terminal" }, std::pair{ "d", "launcher" } })
        if (auto it = config.commands.find(command); it != config.commands.end())
            bind(super, name, Spawn{ it->second });
    bind(super, "q", Kill{ });
    char const* const azerty[] = { "ampersand", "eacute", "quotedbl", "apostrophe", "parenleft",
                                   "minus",     "egrave", "underscore", "ccedilla", "agrave" };
    char const* const digits[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" };
    for (size_t i = 0; i < std::min<size_t>(config.workspaces.count, 10); ++i)
        for (auto const* keys : { azerty, digits })
        {
            bind(super, keys[i], SwitchWorkspace{ i });
            bind(super_shift, keys[i], MoveToWorkspace{ i });
        }
    bind(super, "Left", FocusMonitor{ -1 });
    bind(super, "Right", FocusMonitor{ 1 });
    bind(super_shift, "Left", MoveToMonitor{ -1 });
    bind(super_shift, "Right", MoveToMonitor{ 1 });
    bind(super, "f", ToggleFullscreen{ });
    bind(super_shift, "f", ToggleFloat{ });
    bind(super, "j", FocusCycle{ true });
    bind(super, "k", FocusCycle{ false });
    bind(super, "h", AdjustRatio{ -0.05 });
    bind(super, "l", AdjustRatio{ 0.05 });
}

Config default_values()
{
    Config config;
    config.workspaces.names.resize(config.workspaces.count);
    for (size_t i = 0; i < config.workspaces.names.size(); ++i) config.workspaces.names[i] = std::to_string(i + 1);
    config.commands["terminal"] = CommandConfig::argv_command({ "/usr/local/bin/st" });
    config.commands["browser"] = CommandConfig::argv_command({ "/usr/bin/firefox" });
    config.commands["launcher"] = CommandConfig::argv_command({ "dmenu_run" });
    config.mousebinds = {
        { XCB_MOD_MASK_4, 1, MouseAction::DragWindow },
        { XCB_MOD_MASK_4, 3, MouseAction::ResizeFloating },
        { XCB_MOD_MASK_4, 2, MouseAction::ToggleFloat },
    };
    return config;
}

} // namespace

Config default_config()
{
    auto config = default_values();
    add_default_keybinds(config);
    return config;
}

ConfigLoadResult load_config_result(std::string const& path)
{
    try
    {
        auto root = toml::parse_file(path);
        static constexpr std::pair<std::string_view, bool> sections[] = {
            { "appearance", false }, { "layout", false },         { "focus", false },
            { "commands", false },   { "workspaces", false },     { "autostart", false },
            { "binds", true },       { "workspace_binds", true }, { "mousebinds", true },
            { "rules", true },       { "scratchpads", true },
        };
        for (auto const& [key, _] : root)
            if (std::ranges::none_of(sections, [&](auto const& section) { return section.first == key.str(); }))
                return std::unexpected("top-level config has unknown key '" + std::string(key.str()) + "'");
        for (auto const& [name, array] : sections)
            if (auto const* node = root.get(name); node && (array ? !node->is_array() : !node->is_table()))
                return std::unexpected(
                    (array ? "[[" : "[") + std::string(name) + (array ? "]] must be an array" : "] must be a table")
                );

        // Sections parse in dependency order: bindings and rules refer to
        // commands, workspace names, scratchpads, and ratio bounds.
        Config config = default_values();
        if (auto const* table = root.get_as<toml::table>("appearance"))
            LWM_TRY(parse_appearance(*table, config.appearance));
        if (auto const* table = root.get_as<toml::table>("layout"))
            LWM_TRY(parse_layout(*table, config.layout));
        if (auto const* table = root.get_as<toml::table>("focus"))
            LWM_TRY(parse_focus(*table, config.focus));
        if (auto const* table = root.get_as<toml::table>("commands"))
            LWM_TRY(parse_commands(*table, config.commands));
        if (auto const* table = root.get_as<toml::table>("workspaces"))
            LWM_TRY(parse_workspaces(*table, config.workspaces));
        if (root.contains("scratchpads"))
            config.scratchpads.clear();
        LWM_TRY(for_each_table(
            root,
            "scratchpads",
            "[[scratchpads]]",
            [&](auto const& table, auto const& context) { return parse_scratchpad(table, context, config); }
        ));
        if (auto const* table = root.get_as<toml::table>("autostart"))
            LWM_TRY(parse_autostart(*table, config));

        // [[binds]] starts from an empty key-binding set; otherwise defaults apply.
        bool defaults_retained = !root.contains("binds");
        if (defaults_retained)
            add_default_keybinds(config);
        LWM_TRY(for_each_table(
            root,
            "binds",
            "[[binds]]",
            [&](auto const& table, auto const& context) { return parse_bind(table, context, config); }
        ));
        std::set<std::pair<uint16_t, bool>> replaced;
        LWM_TRY(for_each_table(
            root,
            "workspace_binds",
            "[[workspace_binds]]",
            [&](auto const& table, auto const& context)
            { return parse_workspace_bind(table, context, config, defaults_retained, replaced); }
        ));
        if (root.contains("mousebinds"))
            config.mousebinds.clear();
        LWM_TRY(for_each_table(
            root,
            "mousebinds",
            "[[mousebinds]]",
            [&](auto const& table, auto const& context) { return parse_mousebind(table, context, config); }
        ));
        LWM_TRY(for_each_table(
            root,
            "rules",
            "[[rules]]",
            [&](auto const& table, auto const& context) { return parse_rule(table, context, config); }
        ));
        return config;
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
