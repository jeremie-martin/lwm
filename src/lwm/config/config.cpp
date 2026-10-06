#include "config.hpp"
#include "lwm/core/command.hpp"
#include "lwm/core/overloaded.hpp"
#include <X11/Xlib.h>
// Xlib defines stacking-mode macros that collide with LayerHint enumerators.
#undef Above
#undef Below
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <rfl/Literal.hpp>
#include <rfl/NoExtraFields.hpp>
#include <rfl/Rename.hpp>
#include <rfl/Validator.hpp>
#include <rfl/comparisons.hpp>
#include <rfl/toml/read.hpp>
#include <filesystem>
#include <fstream>
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
    std::vector<std::string> names; ///< The list is the workspace count
};
using Argv = std::vector<std::string>;
struct Match
{
    rfl::Rename<"class", Text> class_name;
    Text instance, title;
};
struct RuleMatch
{
    rfl::Rename<"class", Text> class_name;
    Text instance, title;
    // Only these types become clients; docks, desktops and popups are never ruled.
    std::optional<rfl::Literal<"normal", "dialog", "utility", "toolbar", "menu", "splash">> type;
    Flag transient;
};
struct Size
{
    std::optional<Number> width, height;
};
struct Scratchpad
{
    std::string name;
    Argv spawn;
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
    Flag floating, fullscreen, sticky, skip_taskbar, skip_pager, borderless;
    std::optional<std::variant<Integer<0, 65534>, std::string>> workspace;
    std::optional<std::variant<Integer<0, 2147483647>, std::string>> monitor;
    std::optional<rfl::Literal<"normal", "above", "below">> layer;
    std::optional<Geometry> geometry;
};
struct Rule
{
    std::optional<RuleMatch> match;
    RuleActions apply;
};
// Command text as lwmctl takes it, or the argv of a process to launch.
using BindAction = std::variant<std::string, Argv>;
// One keyboard layout's workspace keys: modifier+key N switches to or moves to workspace N.
struct WorkspaceKeys
{
    rfl::Rename<"switch", Text> switch_mod;
    Text move;
    std::vector<std::string> keys;
};
struct Config
{
    std::optional<Appearance> appearance;
    std::optional<Layout> layout;
    std::optional<Focus> focus;
    std::optional<Workspaces> workspaces;
    std::optional<std::vector<Scratchpad>> scratchpads;
    std::optional<std::map<std::string, BindAction>> binds;
    std::optional<std::vector<WorkspaceKeys>> workspace_keys;
    std::optional<std::map<std::string, BindAction>> mousebinds;
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

// "mod+mod+KEY": the modifiers and the final component.
std::pair<uint16_t, std::string> parse_combo(std::string const& combo, std::string const& context)
{
    auto separator = combo.rfind('+');
    // A leading '+' is an empty modifier, not an unmodified key.
    if (separator == 0)
        throw std::runtime_error(context + " contains an empty modifier");
    if (separator == combo.npos)
        return { 0, combo };
    return { parse_modifiers(std::string_view(combo).substr(0, separator), context), combo.substr(separator + 1) };
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

std::vector<std::string> launch_argv(schema::Argv argv, std::string const& context)
{
    if (argv.empty() || argv.front().empty())
        throw std::runtime_error(context + " must start with a nonempty executable");
    if (std::ranges::any_of(argv, [](auto const& arg) { return arg.contains('\0'); }))
        throw std::runtime_error(context + " must not contain NUL");
    return argv;
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

Action parse_binding(schema::BindAction const& input, std::string const& context, Config const& config)
{
    if (auto const* argv = std::get_if<schema::Argv>(&input))
        return action::Spawn{ launch_argv(*argv, context) };
    auto request = command::parse_command(std::get<std::string>(input));
    if (!request)
        throw std::runtime_error(context + ": " + request.error());
    auto* operation = std::get_if<Action>(&*request);
    if (!operation)
        throw std::runtime_error(context + " must be an operation, not a query");
    // Resolve configuration references now; execution still checks live state.
    std::visit(
        Overloaded{
            [&](action::SwitchWorkspace const& a) { parse_workspace_index(a.workspace, context, config.workspaces.size()); },
            [&](action::MoveToWorkspace const& a) { parse_workspace_index(a.workspace, context, config.workspaces.size()); },
            [&](action::SetRatio const& a) { parse_ratio(a.value, context, config.layout); },
            [&](action::ScratchpadToggle const& a) { parse_scratchpad_name(a.name, context, config); },
            [&](action::ScratchpadCancelLaunch const& a) { parse_scratchpad_name(a.name, context, config); },
            [](auto const&) { }
        },
        *operation
    );
    return std::move(*operation);
}

void parse_scratchpad(schema::Scratchpad const& input, std::string const& context, Config& config)
{
    if (input.name.empty())
        throw std::runtime_error(context + ".name must not be empty");
    if (std::ranges::any_of(config.scratchpads, [&](auto const& s) { return s.name == input.name; }))
        throw std::runtime_error(context + ".name duplicates scratchpad '" + input.name + "'");
    ScratchpadConfig scratchpad;
    scratchpad.name = input.name;
    scratchpad.spawn = launch_argv(input.spawn, context + ".spawn");
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
        if (auto const& type = input.match->type)
            rule.type = std::array{ WindowType::Normal, WindowType::Dialog, WindowType::Utility,
                                    WindowType::Toolbar, WindowType::Menu, WindowType::Splash }[type->value()];
        rule.transient = input.match->transient;
    }
    auto const& in = input.apply;
    auto& out = rule.actions;
    if (!in.floating && !in.fullscreen && !in.layer && !in.sticky && !in.skip_taskbar && !in.skip_pager
        && !in.borderless && !in.workspace && !in.monitor && !in.geometry)
        throw std::runtime_error(context + ".apply must define at least one action");
    if (in.workspace)
    {
        if (auto name = std::get_if<std::string>(&*in.workspace))
        {
            auto it = std::ranges::find(config.workspaces, *name);
            if (it == config.workspaces.end())
                throw std::runtime_error(context + ".apply.workspace points to unknown workspace '" + *name + "'");
            out.workspace = static_cast<size_t>(it - config.workspaces.begin());
        }
        else
            out.workspace = parse_workspace_index(
                std::get<schema::Integer<0, 65534>>(*in.workspace).value(),
                context + ".apply.workspace",
                config.workspaces.size()
            );
    }
    if (in.monitor)
    {
        if (auto name = std::get_if<std::string>(&*in.monitor))
            out.monitor = *name;
        else
            out.monitor = static_cast<size_t>(std::get<schema::Integer<0, 2147483647>>(*in.monitor).value());
    }
    if (in.layer)
        out.layer = std::array{ LayerHint::Normal, LayerHint::Above, LayerHint::Below }[in.layer->value()];
    out.floating = in.floating;
    out.fullscreen = in.fullscreen;
    out.sticky = in.sticky;
    out.skip_taskbar = in.skip_taskbar;
    out.skip_pager = in.skip_pager;
    out.borderless = in.borderless;
    if (auto const& geometry = in.geometry)
    {
        if (geometry->x.has_value() != geometry->y.has_value())
            throw std::runtime_error(context + ".apply.geometry must set both x and y, or neither");
        auto& frame = out.geometry.emplace();
        if (geometry->x)
            frame.position = { static_cast<int16_t>(geometry->x->value()), static_cast<int16_t>(geometry->y->value()) };
        assign(geometry->width, frame.width);
        assign(geometry->height, frame.height);
    }
    config.rules.push_back(std::move(rule));
}

// The file is the whole configuration: only settings it omits take defaults.
ConfigLoadResult parse_config(std::string_view text, std::string const& source)
{
    try
    {
        auto document = toml::parse(text, source);
        auto decoded = rfl::toml::read<schema::Config, rfl::NoExtraFields>(&document);
        if (!decoded)
            throw std::runtime_error(decoded.error().what());
        auto const& input = *decoded;
        Config config;
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
        if (input.workspaces)
        {
            if (input.workspaces->names.empty() || input.workspaces->names.size() > 65535)
                throw std::runtime_error("[workspaces].names must contain 1..65535 entries");
            config.workspaces = input.workspaces->names;
        }
        // Resolve declarations before their use sites. No unresolved input escapes this load.
        for_each(input.scratchpads, "scratchpads", [&](auto const& value, auto const& context) { parse_scratchpad(value, context, config); });
        if (input.binds)
            for (auto const& [combo, action] : *input.binds)
            {
                auto context = "[binds]." + combo;
                auto [modifier, key] = parse_combo(combo, context);
                add_binding(config, { modifier, parse_keysym(key, context) }, parse_binding(action, context, config), context);
            }
        for_each(
            input.workspace_keys,
            "workspace_keys",
            [&](auto const& value, auto const& context)
            {
                if (!value.switch_mod.value() && !value.move)
                    throw std::runtime_error(context + " must define switch, move, or both");
                if (value.keys.size() != config.workspaces.size())
                    throw std::runtime_error(
                        context + ".keys must contain exactly " + std::to_string(config.workspaces.size()) + " entries"
                    );
                for (size_t i = 0; i < value.keys.size(); ++i)
                {
                    auto key_context = context + ".keys[" + std::to_string(i) + "]";
                    auto keysym = parse_keysym(value.keys[i], key_context);
                    if (auto const& mod = value.switch_mod.value())
                        add_binding(config, { parse_modifiers(*mod, context + ".switch"), keysym }, action::SwitchWorkspace{ i }, key_context);
                    if (auto const& mod = value.move)
                        add_binding(config, { parse_modifiers(*mod, context + ".move"), keysym }, action::MoveToWorkspace{ i }, key_context);
                }
            }
        );
        if (input.mousebinds)
            for (auto const& [combo, action] : *input.mousebinds)
            {
                auto context = "[mousebinds]." + combo;
                auto [modifier, button] = parse_combo(combo, context);
                int value = 0;
                auto [end, error] = std::from_chars(button.data(), button.data() + button.size(), value);
                if (error != std::errc{} || end != button.data() + button.size() || value < 1 || value > 255)
                    throw std::runtime_error(context + " must end with a button number 1..255");
                if (std::ranges::any_of(config.mousebinds, [&](auto const& bind) { return bind.modifier == modifier && bind.button == value; }))
                    throw std::runtime_error(context + " duplicates an existing binding");
                auto const* text = std::get_if<std::string>(&action);
                config.mousebinds.push_back({ modifier, static_cast<uint8_t>(value),
                                              text && *text == "move"     ? MouseGrip::Move
                                              : text && *text == "resize" ? MouseGrip::Resize
                                                                          : std::variant<MouseGrip, Action>{ parse_binding(action, context, config) } });
            }
        for_each(
            input.rules,
            "rules",
            [&](auto const& value, auto const& context) { parse_rule(value, context, config); }
        );
        return config;
    }
    catch (std::exception const& error)
    {
        return std::unexpected("Config error in '" + source + "': " + error.what());
    }
}

} // namespace

// The shipped example is the default configuration, so defaults have one home.
Config default_config()
{
    static constexpr char text[] = {
#embed "../../../config.toml.example"
    };
    auto config = parse_config({ text, sizeof text }, "built-in defaults");
    if (!config)
        throw std::logic_error(config.error());
    return std::move(*config);
}

ConfigLoadResult load_config(std::string const& path, bool required)
{
    std::error_code error;
    bool exists = !path.empty() && std::filesystem::exists(path, error);
    if (error)
        return std::unexpected("Cannot inspect config file '" + path + "': " + error.message());
    if (exists)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::unexpected("Cannot read config file '" + path + "'");
        return parse_config(std::string(std::istreambuf_iterator<char>(file), { }), path);
    }
    if (!required)
        return default_config();
    return std::unexpected(path.empty() ? "no config path is configured" : "config file does not exist: " + path);
}

} // namespace lwm
