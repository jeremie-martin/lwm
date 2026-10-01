#include "config.hpp"
#include <X11/Xlib.h>
// Xlib defines stacking-mode macros that collide with LayerHint enumerators.
#undef Above
#undef Below
#include <algorithm>
#include <cctype>
#include <cmath>
#include <rfl/ExtraFields.hpp>
#include <rfl/NoExtraFields.hpp>
#include <rfl/Rename.hpp>
#include <rfl/Validator.hpp>
#include <rfl/comparisons.hpp>
#include <rfl/toml/read.hpp>
#include <set>
#include <stdexcept>
#include <string_view>

namespace lwm {
namespace {

// Only the input schema knows about reflect-cpp. Runtime Config contains resolved
// actions and matchers, never TOML nodes or library-specific wrapper types.
namespace schema {

// Validate in TOML's signed representation, before narrowing to runtime types.
template <int64_t Min, int64_t Max> using Integer = rfl::Validator<int64_t, rfl::Minimum<Min>, rfl::Maximum<Max>>;
template <int64_t Min, int64_t Max> using OptionalInteger = std::optional<Integer<Min, Max>>;
using Text = std::optional<std::string>;
using Flag = std::optional<bool>;

struct Number
{
    using ReflectionType = std::variant<int64_t, double>;
    double value;
    explicit Number(ReflectionType const& input)
        : value(std::visit([](auto v) { return static_cast<double>(v); }, input))
    {
        if (!std::isfinite(value))
            throw std::runtime_error("must be a finite number");
    }
};

struct Appearance
{
    OptionalInteger<0, 65535> padding, border_width;
    OptionalInteger<0, 4294967295> border_color, urgent_border_color;
};
struct Layout
{
    Text strategy;
    std::optional<Number> default_ratio, min_ratio;
    OptionalInteger<1, 100> resize_grab_threshold;
};
struct Focus
{
    Flag warp_cursor_on_monitor_change;
};
struct Workspaces
{
    OptionalInteger<1, 65535> count;
    std::optional<std::vector<std::string>> names;
};
struct Command
{
    Text ref, shell;
    std::optional<std::vector<std::string>> argv;
};
struct Autostart
{
    std::optional<std::vector<Command>> commands;
};
struct Match
{
    rfl::Rename<"class", Text> class_name;
    Text instance, title;
};
struct RuleMatch
{
    rfl::Rename<"class", Text> class_name;
    Text instance, title, type;
    Flag transient;
};
struct Size
{
    std::optional<Number> width, height;
};
struct Scratchpad
{
    std::string name;
    Command spawn;
    Match match;
    std::optional<Size> size;
};
struct Geometry
{
    OptionalInteger<-32768, 32767> x, y;
    OptionalInteger<1, 65535> width, height;
};
struct RuleActions
{
    Flag floating, fullscreen, above, below, sticky, skip_taskbar, skip_pager, borderless, center;
    OptionalInteger<0, 65534> workspace;
    OptionalInteger<0, 2147483647> monitor;
    Text workspace_name, monitor_name, scratchpad;
    std::optional<Geometry> geometry;
};
struct Rule
{
    std::optional<RuleMatch> match;
    RuleActions apply;
};
using Argument = std::variant<bool, int64_t, Number, std::string, Command>;
struct Bind
{
    std::string key;
    rfl::ExtraFields<Argument> actions;
};
struct WorkspaceBind
{
    std::string mode, mod;
    std::vector<std::string> keys;
};
struct MouseBind
{
    Text mod;
    Integer<1, 255> button;
    std::string action;
};
struct Config
{
    std::optional<Appearance> appearance;
    std::optional<Layout> layout;
    std::optional<Focus> focus;
    std::optional<Workspaces> workspaces;
    std::optional<std::map<std::string, Command>> commands;
    std::optional<Autostart> autostart;
    std::optional<std::vector<Scratchpad>> scratchpads;
    std::optional<std::vector<Bind>> binds;
    std::optional<std::vector<WorkspaceBind>> workspace_binds;
    std::optional<std::vector<MouseBind>> mousebinds;
    std::optional<std::vector<Rule>> rules;
};

} // namespace schema

template <typename T, typename Value> void assign(std::optional<Value> const& input, T& output)
{
    if (input)
        output = static_cast<T>(input->value());
}

// Number is also used for settings that accept either TOML integer or float syntax.
void assign(std::optional<schema::Number> const& input, double& output)
{
    if (input)
        output = input->value;
}

template <typename T, typename Handler>
void for_each(std::optional<std::vector<T>> const& input, std::string const& name, Handler handler)
{
    if (input)
        for (size_t i = 0; i < input->size(); ++i) handler((*input)[i], "[[" + name + "]]#" + std::to_string(i));
}

WindowType parse_window_type(std::string type, std::string const& context)
{
    std::ranges::transform(type, type.begin(), [](unsigned char c) { return std::tolower(c); });
    static constexpr std::pair<std::string_view, WindowType> types[] = {
        {       "desktop",      WindowType::Desktop },
        {          "dock",         WindowType::Dock },
        {       "toolbar",      WindowType::Toolbar },
        {          "menu",         WindowType::Menu },
        {       "utility",      WindowType::Utility },
        {        "splash",       WindowType::Splash },
        {        "dialog",       WindowType::Dialog },
        { "dropdown_menu", WindowType::DropdownMenu },
        {  "dropdownmenu", WindowType::DropdownMenu },
        {    "popup_menu",    WindowType::PopupMenu },
        {     "popupmenu",    WindowType::PopupMenu },
        {       "tooltip",      WindowType::Tooltip },
        {  "notification", WindowType::Notification },
        {         "combo",        WindowType::Combo },
        {           "dnd",          WindowType::Dnd },
        {        "normal",       WindowType::Normal },
    };
    for (auto const& [name, value] : types)
        if (name == type)
            return value;
    throw std::runtime_error(context + " has unknown window type '" + type + "'");
}

uint16_t parse_modifiers(std::string_view text, std::string const& context)
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
            throw std::runtime_error(context + " has unknown or empty modifier '" + std::string(token) + "'");
        if (result & mask)
            throw std::runtime_error(context + " repeats modifier '" + std::string(token) + "'");
        result |= mask;
        if (end == text.npos)
            return result;
        start = end + 1;
    }
}

xcb_keysym_t parse_keysym(std::string const& key, std::string const& context)
{
    auto symbol = XStringToKeysym(key.c_str());
    if (symbol == NoSymbol)
        throw std::runtime_error(context + " has unknown key '" + key + "'");
    return static_cast<xcb_keysym_t>(symbol);
}

KeyBinding parse_key_combo(std::string const& combo, std::string const& context)
{
    auto separator = combo.rfind('+');
    // A leading '+' is an empty modifier, not an unmodified key.
    if (separator == 0)
        throw std::runtime_error(context + " contains an empty modifier");
    auto modifier = parse_modifiers(
        separator == combo.npos ? std::string_view{} : std::string_view(combo).substr(0, separator),
        context
    );
    auto keysym = parse_keysym(combo.substr(separator == combo.npos ? 0 : separator + 1), context);
    return KeyBinding{ modifier, keysym };
}

template <typename Match> WindowMatcher parse_matchers(Match const& input, std::string const& context)
{
    auto pattern = [&](schema::Text const& text, char const* key) -> std::optional<std::regex>
    {
        if (!text)
            return std::nullopt;
        if (text->empty())
            throw std::runtime_error(context + "." + key + " must not be empty");
        try
        {
            return std::regex("^(?:" + *text + ")$", std::regex::ECMAScript | std::regex::optimize);
        }
        catch (std::regex_error const& error)
        {
            throw std::runtime_error(context + "." + key + " has invalid regex: " + error.what());
        }
    };
    return { pattern(input.class_name.value(), "class"),
             pattern(input.instance, "instance"),
             pattern(input.title, "title") };
}

CommandConfig parse_command(
    schema::Command const& input,
    std::string const& context,
    std::map<std::string, CommandConfig> const& registry,
    bool allow_ref = true
)
{
    if (input.ref.has_value() + input.shell.has_value() + input.argv.has_value() != 1)
        throw std::runtime_error(context + " must contain exactly one of 'ref', 'shell', or 'argv'");
    if (input.ref)
    {
        if (!allow_ref)
            throw std::runtime_error(context + ".ref is only allowed at command use sites");
        auto it = registry.find(*input.ref);
        if (it == registry.end())
            throw std::runtime_error(context + ".ref points to unknown command '" + *input.ref + "'");
        return it->second;
    }
    if (input.shell)
    {
        if (input.shell->empty())
            throw std::runtime_error(context + ".shell must not be empty");
        return CommandConfig::shell_command(*input.shell);
    }
    if (input.argv->empty() || input.argv->front().empty())
        throw std::runtime_error(context + ".argv must contain a nonempty executable");
    return CommandConfig::argv_command(*input.argv);
}

size_t parse_workspace_index(int64_t index, std::string const& context, size_t count)
{
    if (index < 0 || static_cast<uint64_t>(index) >= count)
        throw std::runtime_error(context + " must be in range 0.." + std::to_string(count - 1));
    return static_cast<size_t>(index);
}

std::string parse_scratchpad_name(std::string const& name, std::string const& context, Config const& config)
{
    if (std::ranges::none_of(config.scratchpads, [&](auto const& scratchpad) { return scratchpad.name == name; }))
        throw std::runtime_error(context + " points to unknown scratchpad '" + name + "'");
    return name;
}

double parse_ratio(double value, std::string const& context, LayoutConfig const& layout)
{
    if (!layout.accepts_ratio(value))
        throw std::runtime_error(context + " must respect min_ratio bounds");
    return value;
}

template <typename T> T const& argument(schema::Argument const& value, std::string const& context)
{
    if (auto result = std::get_if<T>(&value))
        return *result;
    throw std::runtime_error(context + " has the wrong action argument type");
}

double number(schema::Argument const& value, std::string const& context)
{
    if (auto integer = std::get_if<int64_t>(&value))
        return static_cast<double>(*integer);
    return argument<schema::Number>(value, context).value;
}

Action parse_bind_action(
    std::string_view name,
    schema::Argument const& value,
    std::string const& context,
    Config const& config
)
{
    std::string const field = context + "." + std::string(name);
    using namespace action;
    // Actions without a value are enabled with `name = true`; names come from action_name().
    static Action const flags[] = {
        Kill{},
        ReloadConfig{},
        Restart{},
        ToggleFullscreen{},
        ToggleFloat{},
        FocusCycle{ true },
        FocusCycle{ false },
        ToggleWorkspace{},
        CycleWorkspace{ 1 },
        CycleWorkspace{ -1 },
        SwapTile{ 1 },
        SwapTile{ -1 },
        ResetRatios{},
        ScratchpadStash{},
        ScratchpadCycle{},
    };
    for (auto const& action : flags)
    {
        if (action_name(action) != name)
            continue;
        auto enabled = argument<bool>(value, field);
        if (!enabled)
            throw std::runtime_error(field + " must be true when present");
        return action;
    }
    if (name == "spawn")
    {
        auto command = parse_command(argument<schema::Command>(value, field), field, config.commands);
        return Spawn{ std::move(command) };
    }
    if (name == "exec")
    {
        auto binary = argument<std::string>(value, field);
        if (binary.empty())
            throw std::runtime_error(field + " must not be empty");
        return Exec{ std::move(binary) };
    }
    if (name == "switch_workspace" || name == "move_to_workspace")
    {
        auto workspace = parse_workspace_index(argument<int64_t>(value, field), field, config.workspaces.count);
        return name == "switch_workspace" ? Action{ SwitchWorkspace{ workspace } }
                                          : Action{ MoveToWorkspace{ workspace } };
    }
    if (name == "focus_monitor" || name == "move_to_monitor")
    {
        auto direction = argument<int64_t>(value, field);
        if (direction != -1 && direction != 1)
            throw std::runtime_error(field + " must be -1 or 1");
        int step = static_cast<int>(direction);
        return name == "focus_monitor" ? Action{ FocusMonitor{ step } } : Action{ MoveToMonitor{ step } };
    }
    if (name == "toggle_scratchpad" || name == "cancel_scratchpad_launch")
    {
        auto scratchpad = parse_scratchpad_name(argument<std::string>(value, field), field, config);
        return name == "toggle_scratchpad" ? Action{ ScratchpadToggle{ std::move(scratchpad) } }
                                           : Action{ ScratchpadCancelLaunch{ std::move(scratchpad) } };
    }
    if (name == "set_layout")
    {
        auto text = argument<std::string>(value, field);
        auto strategy = parse_layout_strategy(text);
        if (!strategy)
            throw std::runtime_error(
                field + " has unknown value '" + text + "' (expected 'master-stack' or 'monocle')"
            );
        return SetLayout{ *strategy };
    }
    if (name == "set_ratio")
    {
        auto ratio = parse_ratio(number(value, field), field, config.layout);
        return SetRatio{ ratio };
    }
    if (name == "adjust_ratio")
    {
        auto delta = number(value, field);
        return AdjustRatio{ delta };
    }
    throw std::runtime_error(context + " has unknown key '" + std::string(name) + "'");
}

void parse_scratchpad(schema::Scratchpad const& input, std::string const& context, Config& config)
{
    if (input.name.empty())
        throw std::runtime_error(context + ".name must not be empty");
    if (std::ranges::any_of(config.scratchpads, [&](auto const& s) { return s.name == input.name; }))
        throw std::runtime_error(context + ".name duplicates scratchpad '" + input.name + "'");
    ScratchpadConfig scratchpad;
    scratchpad.name = input.name;
    scratchpad.spawn = parse_command(input.spawn, context + ".spawn", config.commands);
    scratchpad.match = parse_matchers(input.match, context + ".match");
    if (scratchpad.match.empty())
        throw std::runtime_error(context + ".match must define at least one matcher");
    if (input.size)
    {
        assign(input.size->width, scratchpad.width);
        assign(input.size->height, scratchpad.height);
        if (scratchpad.width < 0.1 || scratchpad.width > 1.0 || scratchpad.height < 0.1 || scratchpad.height > 1.0)
            throw std::runtime_error(context + ".size fractions must be in range 0.1..1.0");
    }
    config.scratchpads.push_back(std::move(scratchpad));
}

void add_binding(Config& config, KeyBinding binding, Action action, std::string const& context)
{
    if (!config.keybinds.emplace(binding, std::move(action)).second)
        throw std::runtime_error(context + " duplicates an existing binding");
}

void parse_rule(schema::Rule const& input, std::string const& context, Config& config)
{
    WindowRuleConfig rule;
    if (input.match)
    {
        rule.match = parse_matchers(*input.match, context + ".match");
        if (input.match->type)
            rule.type = parse_window_type(*input.match->type, context + ".match.type");
        rule.transient = input.match->transient;
    }
    auto const& in = input.apply;
    auto& out = rule.actions;
    if (!in.floating && !in.fullscreen && !in.above && !in.below && !in.sticky && !in.skip_taskbar && !in.skip_pager
        && !in.borderless && !in.center && !in.workspace && !in.workspace_name && !in.monitor && !in.monitor_name
        && !in.scratchpad && !in.geometry)
        throw std::runtime_error(context + ".apply must define at least one action");
    if (in.workspace && in.workspace_name)
        throw std::runtime_error(context + ".apply cannot define both 'workspace' and 'workspace_name'");
    if (in.monitor && in.monitor_name)
        throw std::runtime_error(context + ".apply cannot define both 'monitor' and 'monitor_name'");
    if (in.workspace)
        out.workspace =
            parse_workspace_index(in.workspace->value(), context + ".apply.workspace", config.workspaces.count);
    if (in.workspace_name)
    {
        auto it = std::ranges::find(config.workspaces.names, *in.workspace_name);
        if (it == config.workspaces.names.end())
            throw std::runtime_error(
                context + ".apply.workspace_name points to unknown workspace '" + *in.workspace_name + "'"
            );
        out.workspace = static_cast<size_t>(it - config.workspaces.names.begin());
    }
    if (in.monitor)
        out.monitor = static_cast<size_t>(in.monitor->value());
    if (in.monitor_name)
        out.monitor = *in.monitor_name;
    if (in.above.value_or(false) && in.below.value_or(false))
        throw std::runtime_error(context + ".apply cannot set both 'above' and 'below' to true");
    if (in.above || in.below)
        out.layer = in.above.value_or(false) ? LayerHint::Above
            : in.below.value_or(false)       ? LayerHint::Below
                                             : LayerHint::Normal;
    out.floating = in.floating;
    out.fullscreen = in.fullscreen;
    out.sticky = in.sticky;
    out.skip_taskbar = in.skip_taskbar;
    out.skip_pager = in.skip_pager;
    out.borderless = in.borderless;
    out.center = in.center.value_or(false);
    if (in.geometry)
    {
        Geometry geometry{ 0, 0, 800, 600 };
        assign(in.geometry->x, geometry.x);
        assign(in.geometry->y, geometry.y);
        assign(in.geometry->width, geometry.width);
        assign(in.geometry->height, geometry.height);
        out.geometry = geometry;
    }
    if (in.scratchpad)
        out.scratchpad = parse_scratchpad_name(*in.scratchpad, context + ".apply.scratchpad", config);
    config.rules.push_back(std::move(rule));
}

void add_default_keybinds(Config& config)
{
    using namespace action;
    auto key = [](char const* name) { return static_cast<xcb_keysym_t>(XStringToKeysym(name)); };
    auto bind = [&](uint16_t mod, char const* name, Action action)
    { config.keybinds[{ mod, key(name) }] = std::move(action); };
    uint16_t const super = XCB_MOD_MASK_4;
    uint16_t const super_shift = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT;

    for (auto [name, command] : {
             std::pair{ "Return", "terminal" },
             std::pair{      "d", "launcher" }
    })
        if (auto it = config.commands.find(command); it != config.commands.end())
            bind(super, name, Spawn{ it->second });
    bind(super, "q", Kill{});
    char const* const azerty[] = { "ampersand", "eacute", "quotedbl",   "apostrophe", "parenleft",
                                   "minus",     "egrave", "underscore", "ccedilla",   "agrave" };
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
    bind(super, "f", ToggleFullscreen{});
    bind(super_shift, "f", ToggleFloat{});
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
        { XCB_MOD_MASK_4, 1,     MouseAction::DragWindow },
        { XCB_MOD_MASK_4, 3, MouseAction::ResizeFloating },
        { XCB_MOD_MASK_4, 2,    MouseAction::ToggleFloat },
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
        auto document = toml::parse_file(path);
        auto decoded = rfl::toml::read<schema::Config, rfl::NoExtraFields>(&document);
        if (!decoded)
            throw std::runtime_error(decoded.error().what());
        auto const& input = *decoded;
        Config config = default_values();
        if (auto const& appearance = input.appearance)
        {
            assign(appearance->padding, config.appearance.padding);
            assign(appearance->border_width, config.appearance.border_width);
            assign(appearance->border_color, config.appearance.border_color);
            assign(appearance->urgent_border_color, config.appearance.urgent_border_color);
        }
        if (auto const& layout = input.layout)
        {
            if (layout->strategy)
            {
                auto strategy = parse_layout_strategy(*layout->strategy);
                if (!strategy)
                    throw std::runtime_error("[layout].strategy has unknown value '" + *layout->strategy + "'");
                config.layout.strategy = *strategy;
            }
            assign(layout->min_ratio, config.layout.min_ratio);
            if (config.layout.min_ratio < 0.05 || config.layout.min_ratio > 0.45)
                throw std::runtime_error("[layout].min_ratio must be in range 0.05..0.45");
            if (layout->default_ratio)
                config.layout.default_ratio =
                    parse_ratio(layout->default_ratio->value, "[layout].default_ratio", config.layout);
            assign(layout->resize_grab_threshold, config.layout.resize_grab_threshold);
        }
        if (input.focus)
            config.focus.warp_cursor_on_monitor_change = input.focus->warp_cursor_on_monitor_change.value_or(false);
        if (input.commands)
            for (auto const& [name, command] : *input.commands)
                config.commands[name] = parse_command(command, "[commands]." + name, config.commands, false);
        if (input.workspaces)
        {
            auto names = input.workspaces->names.value_or(std::vector<std::string>{});
            if (names.size() > 65535)
                throw std::runtime_error("[workspaces].names cannot contain more than 65535 entries");
            if (!names.empty())
                config.workspaces.count = names.size();
            assign(input.workspaces->count, config.workspaces.count);
            size_t named = std::min(names.size(), config.workspaces.count);
            names.resize(config.workspaces.count);
            for (size_t i = named; i < names.size(); ++i) names[i] = std::to_string(i + 1);
            config.workspaces.names = std::move(names);
        }
        // Resolve declarations before their use sites. No unresolved input escapes this load.
        for_each(
            input.scratchpads,
            "scratchpads",
            [&](auto const& value, auto const& context) { parse_scratchpad(value, context, config); }
        );
        if (input.autostart)
            for_each(
                input.autostart->commands,
                "autostart.commands",
                [&](auto const& value, auto const& context)
                { config.autostart.commands.push_back(parse_command(value, context, config.commands)); }
            );
        if (!input.binds)
            add_default_keybinds(config);
        for_each(
            input.binds,
            "binds",
            [&](auto const& value, auto const& context)
            {
                if (value.actions.size() != 1)
                    throw std::runtime_error(context + " must define exactly one action");
                auto const& [name, argument] = *value.actions.begin();
                add_binding(
                    config,
                    parse_key_combo(value.key, context + ".key"),
                    parse_bind_action(name, argument, context, config),
                    context
                );
            }
        );
        std::set<std::pair<uint16_t, bool>> replaced;
        for_each(
            input.workspace_binds,
            "workspace_binds",
            [&](auto const& value, auto const& context)
            {
                if (value.mode != "switch" && value.mode != "move")
                    throw std::runtime_error(context + ".mode must be 'switch' or 'move'");
                bool move = value.mode == "move";
                auto mod = parse_modifiers(value.mod, context + ".mod");
                if (value.keys.size() != config.workspaces.count)
                    throw std::runtime_error(
                        context + ".keys must contain exactly " + std::to_string(config.workspaces.count) + " entries"
                    );
                if (!input.binds && replaced.insert({ mod, move }).second)
                    std::erase_if(
                        config.keybinds,
                        [&](auto const& binding)
                        {
                            return binding.first.modifier == mod
                                && (move ? std::holds_alternative<action::MoveToWorkspace>(binding.second)
                                         : std::holds_alternative<action::SwitchWorkspace>(binding.second));
                        }
                    );
                for (size_t i = 0; i < value.keys.size(); ++i)
                {
                    auto key_context = context + ".keys[" + std::to_string(i) + "]";
                    Action action =
                        move ? Action{ action::MoveToWorkspace{ i } } : Action{ action::SwitchWorkspace{ i } };
                    add_binding(
                        config,
                        { mod, parse_keysym(value.keys[i], key_context) },
                        std::move(action),
                        key_context
                    );
                }
            }
        );
        if (input.mousebinds)
            config.mousebinds.clear();
        for_each(
            input.mousebinds,
            "mousebinds",
            [&](auto const& value, auto const& context)
            {
                MouseAction action;
                if (value.action == "drag_window")
                    action = MouseAction::DragWindow;
                else if (value.action == "resize_floating")
                    action = MouseAction::ResizeFloating;
                else if (value.action == "toggle_float")
                    action = MouseAction::ToggleFloat;
                else
                    throw std::runtime_error(context + ".action has unknown mouse action '" + value.action + "'");
                config.mousebinds.push_back({ parse_modifiers(value.mod.value_or(""), context + ".mod"),
                                              static_cast<uint8_t>(value.button.value()),
                                              action });
            }
        );
        for_each(
            input.rules,
            "rules",
            [&](auto const& value, auto const& context) { parse_rule(value, context, config); }
        );
        return config;
    }
    catch (std::exception const& error)
    {
        return std::unexpected("Config error in '" + path + "': " + error.what());
    }
}

} // namespace lwm
