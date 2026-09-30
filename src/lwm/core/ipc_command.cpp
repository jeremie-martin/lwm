#include "ipc_command.hpp"
#include "events.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace lwm::ipc {
namespace {
using namespace lwm::action;

template <auto query> Request make_query(Value const&) { return query; }
template <auto action> Request make_action(Value const&) { return Action{ action }; }
Request make_subscribe(Value const& value) { return Subscribe{ std::get<uint32_t>(value) }; }
Request make_exec(Value const& value) { return Action{ Exec{ std::get<std::string>(value) } }; }
Request make_layout(Value const& value) { return Action{ SetLayout{ std::get<LayoutStrategy>(value) } }; }
Request make_set_ratio(Value const& value) { return Action{ SetRatio{ std::get<double>(value) } }; }
Request make_adjust_ratio(Value const& value) { return Action{ AdjustRatio{ std::get<double>(value) } }; }
Request make_attention(Value const& value) { return Action{ NotifyAttention{ std::get<uint32_t>(value) } }; }
Request make_switch(Value const& value) { return Action{ SwitchWorkspace{ std::get<uint32_t>(value) } }; }
Request make_move(Value const& value) { return Action{ MoveToWorkspace{ std::get<uint32_t>(value) } }; }
Request make_focus_window(Value const& value) { return Action{ FocusWindow{ std::get<uint32_t>(value) } }; }
Request make_focus_monitor(Value const& value) { return Action{ FocusMonitor{ std::get<int>(value) } }; }
Request make_move_monitor(Value const& value) { return Action{ MoveToMonitor{ std::get<int>(value) } }; }
Request make_toggle(Value const& value) { return Action{ ScratchpadToggle{ std::get<std::string>(value) } }; }
Request make_cancel(Value const& value) { return Action{ ScratchpadCancelLaunch{ std::get<std::string>(value) } }; }

// Spawn is the only action without an IPC spelling: IPC callers can start processes themselves.
CommandSpec const specs[] = {
    { "ping", Argument::None, "ping", "check whether the WM is running", make_query<Query::Ping> },
    { "version", Argument::None, "version", "show WM version", make_query<Query::Version> },
    { "log status", Argument::None, "log status", "show logging configuration and backend notifications as JSON", make_query<Query::LogStatus> },
    { "state", Argument::None, "state", "print one consistent state snapshot", make_query<Query::State> },
    { "subscribe", Argument::Filter, "subscribe [FILTER]", "stream filtered JSON events", make_subscribe },
    { "reload-config", Argument::None, "reload-config", "reload configuration", make_action<ReloadConfig{ }> },
    { "restart", Argument::None, "restart", "restart the WM", make_action<Restart{ }> },
    { "exec", Argument::Text, "exec PATH", "restart with another binary", make_exec },
    { "layout set", Argument::Layout, "layout set NAME", "select master-stack or monocle", make_layout },
    { "ratio set", Argument::Number, "ratio set VALUE", "set the root split ratio", make_set_ratio },
    { "ratio reset", Argument::None, "ratio reset", "reset workspace split ratios", make_action<ResetRatios{ }> },
    { "ratio adjust", Argument::Number, "ratio adjust DELTA", "adjust the root split ratio", make_adjust_ratio },
    { "notify-attention", Argument::Window, "notify-attention window=<xid>", "mark an exact window urgent", make_attention },
    { "workspace switch", Argument::Index, "workspace switch N", "switch workspace (zero-based)", make_switch },
    { "workspace next", Argument::None, "workspace next", "switch to the next workspace", make_action<CycleWorkspace{ 1 }> },
    { "workspace prev", Argument::None, "workspace prev", "switch to the previous workspace", make_action<CycleWorkspace{ -1 }> },
    { "workspace toggle", Argument::None, "workspace toggle", "switch back to the previous workspace", make_action<ToggleWorkspace{ }> },
    { "workspace list", Argument::None, "workspace list", "print workspaces as JSON", make_query<Query::WorkspaceList> },
    { "monitor focus", Argument::Direction, "monitor focus left|right", "focus the adjacent monitor", make_focus_monitor },
    { "focus next", Argument::None, "focus next", "focus the next MRU window", make_action<FocusCycle{ true }> },
    { "focus prev", Argument::None, "focus prev", "focus the previous MRU window", make_action<FocusCycle{ false }> },
    { "focus", Argument::Window, "focus window=<xid>", "focus an exact window", make_focus_window },
    { "window list", Argument::None, "window list", "print normal clients as JSON", make_query<Query::WindowList> },
    { "window close", Argument::None, "window close", "close the active window", make_action<Kill{ }> },
    { "window fullscreen", Argument::None, "window fullscreen", "toggle fullscreen on the active window", make_action<ToggleFullscreen{ }> },
    { "window float", Argument::None, "window float", "toggle floating on the active window", make_action<ToggleFloat{ }> },
    { "window swap next", Argument::None, "window swap next", "swap the active tile with the next one", make_action<SwapTile{ 1 }> },
    { "window swap prev", Argument::None, "window swap prev", "swap the active tile with the previous one", make_action<SwapTile{ -1 }> },
    { "window to-workspace", Argument::Index, "window to-workspace N", "move the active window to workspace N", make_move },
    { "window to-monitor", Argument::Direction, "window to-monitor left|right", "move the active window to the adjacent monitor", make_move_monitor },
    { "scratchpad stash", Argument::None, "scratchpad stash", "stash the active window", make_action<ScratchpadStash{ }> },
    { "scratchpad cycle", Argument::None, "scratchpad cycle", "cycle the scratchpad pool", make_action<ScratchpadCycle{ }> },
    { "scratchpad toggle", Argument::Text, "scratchpad toggle NAME", "toggle a named scratchpad", make_toggle },
    { "scratchpad cancel-launch", Argument::Text, "scratchpad cancel-launch NAME", "cancel pending launch state", make_cancel },
    { "scratchpad list", Argument::None, "scratchpad list", "print scratchpads as JSON", make_query<Query::ScratchpadList> },
};

constexpr std::string_view spaces = " \t\r\n\v\f";

std::string_view trim(std::string_view text)
{
    auto first = text.find_first_not_of(spaces);
    if (first == text.npos)
        return { };
    return text.substr(first, text.find_last_not_of(spaces) - first + 1);
}

bool matches(std::string_view text, std::string_view name)
{
    return text == name || (text.starts_with(name) && text.size() > name.size() && spaces.contains(text[name.size()]));
}

std::expected<Value, std::string> parse_value(CommandSpec const& spec, std::string_view value)
{
    auto usage = [&] { return std::unexpected("usage: " + std::string(spec.usage)); };
    switch (spec.argument)
    {
        case Argument::None:
            if (!value.empty())
                return usage();
            return Value{ };
        case Argument::Text:
            if (value.empty())
                return usage();
            return Value{ std::string(value) };
        case Argument::Layout:
            if (value.empty())
                return usage();
            if (auto strategy = parse_layout_strategy(value))
                return Value{ *strategy };
            return std::unexpected("unknown layout: " + std::string(value));
        case Argument::Direction:
            if (value == "left")
                return Value{ -1 };
            if (value == "right")
                return Value{ 1 };
            return usage();
        case Argument::Filter:
        {
            auto mask = parse_event_filter(value);
            if (!mask)
                return std::unexpected("no recognized event types in filter");
            return Value{ mask };
        }
        case Argument::Number:
        {
            auto digits = value;
            if (digits.starts_with('+'))
                digits.remove_prefix(1);
            if (digits.empty())
                return usage();
            double number = 0;
            auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
            if (error != std::errc{ } || end != digits.data() + digits.size() || !std::isfinite(number))
                return std::unexpected(
                    std::string(spec.name == "ratio set" ? "invalid ratio value: " : "invalid delta value: ")
                    + std::string(value)
                );
            return Value{ number };
        }
        case Argument::Window:
        case Argument::Index:
        {
            bool window = spec.argument == Argument::Window;
            if (value.empty() || value.find_first_of(spaces) != value.npos)
                return usage();
            if (window)
            {
                if (!value.starts_with("window="))
                    return usage();
                value.remove_prefix(7);
            }
            auto digits = value;
            int base = 10;
            if (window && (digits.starts_with("0x") || digits.starts_with("0X")))
            {
                digits.remove_prefix(2);
                base = 16;
            }
            uint32_t number = 0;
            auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number, base);
            if (digits.empty() || error != std::errc{ } || end != digits.data() + digits.size())
                return std::unexpected(
                    std::string(window ? "invalid window id: " : "invalid workspace index: ") + std::string(value)
                );
            return Value{ number };
        }
    }
    return usage();
}

CommandSpec const* find_spec(std::string_view text)
{
    CommandSpec const* spec = nullptr;
    for (auto const& candidate : specs)
        if (matches(text, candidate.name) && (!spec || candidate.name.size() > spec->name.size()))
            spec = &candidate;
    return spec;
}

} // namespace

std::span<CommandSpec const> command_specs() { return specs; }

std::expected<Request, std::string> parse_command(std::string_view text)
{
    if (text.size() + 1 >= max_request_bytes)
        return std::unexpected("request too large");
    text = trim(text);
    if (text.find_first_of(std::string_view("\n\r\0", 3)) != text.npos)
        return std::unexpected("command contains a line break or NUL");
    if (text.empty())
        return std::unexpected("empty command");
    auto const* spec = find_spec(text);
    if (!spec)
        return std::unexpected("unknown command");
    auto value = parse_value(*spec, trim(text.substr(spec->name.size())));
    if (!value)
        return std::unexpected(value.error());
    return spec->make(*value);
}

std::expected<std::string, std::string> encode_command(std::span<std::string const> arguments)
{
    std::string text;
    for (auto const& argument : arguments)
    {
        if (argument.find_first_of(std::string_view("\n\r\0", 3)) != argument.npos)
            return std::unexpected("argument contains a line break or NUL");
        if (!text.empty())
            text += arguments.front() == "subscribe" && text != "subscribe" ? ',' : ' ';
        text += argument;
    }
    auto parsed = parse_command(text);
    if (!parsed)
        return std::unexpected(parsed.error());
    auto const& spec = *find_spec(trim(text));
    size_t words = 1 + std::count(spec.name.begin(), spec.name.end(), ' ');
    if (spec.argument != Argument::Filter && arguments.size() != words + (spec.argument != Argument::None))
        return std::unexpected("usage: " + std::string(spec.usage));
    return text;
}

} // namespace lwm::ipc
