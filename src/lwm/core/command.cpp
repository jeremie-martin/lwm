#include "command.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace lwm::command {
namespace {
using namespace lwm::action;
constexpr std::string_view spaces = " \t\r\n\v\f";

auto usage(CommandSpec const& spec) { return std::unexpected("usage: " + std::string(spec.usage)); }

std::expected<std::string, std::string> text(std::string_view value, CommandSpec const& spec)
{
    if (value.empty()) return usage(spec);
    return std::string(value);
}

std::expected<LayoutStrategy, std::string> layout(std::string_view value, CommandSpec const& spec)
{
    if (value.empty()) return usage(spec);
    if (auto strategy = parse_layout_strategy(value)) return *strategy;
    return std::unexpected("unknown layout: " + std::string(value));
}

std::expected<int, std::string> direction(std::string_view value, CommandSpec const& spec)
{
    if (value == "left") return -1;
    if (value == "right") return 1;
    return usage(spec);
}

std::expected<double, std::string> number(std::string_view value, CommandSpec const& spec)
{
    auto digits = value;
    if (digits.starts_with('+')) digits.remove_prefix(1);
    if (digits.empty()) return usage(spec);
    double result = 0;
    auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), result);
    if (error != std::errc{} || end != digits.data() + digits.size() || !std::isfinite(result))
        return std::unexpected(
            std::string(spec.name == "ratio set" ? "invalid ratio value: " : "invalid delta value: ") + std::string(value)
        );
    return result;
}

template <bool Window> std::expected<uint32_t, std::string> integer(std::string_view value, CommandSpec const& spec)
{
    if (value.empty() || value.find_first_of(spaces) != value.npos) return usage(spec);
    if constexpr (Window)
    {
        if (!value.starts_with("window=")) return usage(spec);
        value.remove_prefix(7);
    }
    auto digits = value;
    int base = 10;
    if (Window && (digits.starts_with("0x") || digits.starts_with("0X")))
    {
        digits.remove_prefix(2);
        base = 16;
    }
    uint32_t result = 0;
    auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), result, base);
    if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size())
        return std::unexpected(
            std::string(Window ? "invalid window id: " : "invalid workspace index: ") + std::string(value)
        );
    return result;
}

// Each declaration binds its typed parser to the request constructor. CLI arity
// follows that declaration; no intermediate value or runtime extraction is needed.
template <typename T, auto Parse>
constexpr CommandSpec takes(std::string_view name, std::string_view help, std::string_view description)
{
    return { name, 1, help, description,
             [](std::string_view value, CommandSpec const& spec) -> std::expected<Request, std::string>
             {
                 return Parse(value, spec).transform([](auto parsed) { return Request{ T{ std::move(parsed) } }; });
             } };
}

template <typename T, auto... Arguments>
constexpr CommandSpec fixed(std::string_view name, std::string_view help, std::string_view description)
{
    return { name, 0, help, description,
             [](std::string_view value, CommandSpec const& spec) -> std::expected<Request, std::string>
             {
                 if (!value.empty()) return usage(spec);
                 return Request{ T{ Arguments... } };
             } };
}

// Spawn is the only action without an IPC spelling: IPC callers can start processes themselves.
constexpr CommandSpec specs[] = {
    fixed<Query, Query::Ping>("ping", "ping", "check whether the WM is running"),
    fixed<Query, Query::Version>("version", "version", "show WM version"),
    fixed<Query, Query::State>("state", "state", "print one consistent state snapshot"),
    fixed<ReloadConfig>("reload-config", "reload-config", "reload configuration"),
    fixed<Restart>("restart", "restart", "restart the WM"),
    takes<Restart, text>("exec", "exec PATH", "restart with another binary"),
    takes<SetLayout, layout>("layout set", "layout set NAME", "select master-stack or monocle"),
    takes<SetRatio, number>("ratio set", "ratio set VALUE", "set the root split ratio"),
    fixed<ResetRatios>("ratio reset", "ratio reset", "reset workspace split ratios"),
    takes<AdjustRatio, number>("ratio adjust", "ratio adjust DELTA", "adjust the root split ratio"),
    takes<NotifyAttention, integer<true>>("notify-attention", "notify-attention window=<xid>", "mark an exact window urgent"),
    takes<SwitchWorkspace, integer<false>>("workspace switch", "workspace switch N", "switch workspace (zero-based)"),
    fixed<CycleWorkspace, 1>("workspace next", "workspace next", "switch to the next workspace"),
    fixed<CycleWorkspace, -1>("workspace prev", "workspace prev", "switch to the previous workspace"),
    fixed<ToggleWorkspace>("workspace toggle", "workspace toggle", "switch back to the previous workspace"),
    fixed<Query, Query::WorkspaceList>("workspace list", "workspace list", "print workspaces as JSON"),
    takes<FocusMonitor, direction>("monitor focus", "monitor focus left|right", "focus the adjacent monitor"),
    fixed<FocusCycle, true>("focus next", "focus next", "focus the next MRU window"),
    fixed<FocusCycle, false>("focus prev", "focus prev", "focus the previous MRU window"),
    takes<FocusWindow, integer<true>>("focus", "focus window=<xid>", "focus an exact window"),
    fixed<Query, Query::WindowList>("window list", "window list", "print normal clients as JSON"),
    fixed<Kill>("window close", "window close", "close the active window"),
    fixed<ToggleFullscreen>("window fullscreen", "window fullscreen", "toggle fullscreen on the active window"),
    fixed<ToggleFloat>("window float", "window float", "toggle floating on the active window"),
    fixed<SwapTile, 1>("window swap next", "window swap next", "swap the active tile with the next one"),
    fixed<SwapTile, -1>("window swap prev", "window swap prev", "swap the active tile with the previous one"),
    takes<MoveToWorkspace, integer<false>>("window to-workspace", "window to-workspace N", "move the active window to workspace N"),
    takes<MoveToMonitor, direction>("window to-monitor", "window to-monitor left|right", "move the active window to the adjacent monitor"),
    fixed<ScratchpadStash>("scratchpad stash", "scratchpad stash", "stash the active window"),
    fixed<ScratchpadCycle>("scratchpad cycle", "scratchpad cycle", "cycle the scratchpad pool"),
    takes<ScratchpadToggle, text>("scratchpad toggle", "scratchpad toggle NAME", "toggle a named scratchpad"),
    takes<ScratchpadCancelLaunch, text>("scratchpad cancel-launch", "scratchpad cancel-launch NAME", "cancel pending launch state"),
    fixed<Query, Query::ScratchpadList>("scratchpad list", "scratchpad list", "print scratchpads as JSON"),
};

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
    if (text.size() >= max_request_bytes)
        return std::unexpected("request too large");
    text = trim(text);
    if (text.find_first_of(std::string_view("\n\r\0", 3)) != text.npos)
        return std::unexpected("command contains a line break or NUL");
    if (text.empty())
        return std::unexpected("empty command");
    auto const* spec = find_spec(text);
    if (!spec)
        return std::unexpected("unknown command");
    return spec->parse(trim(text.substr(spec->name.size())), *spec);
}

std::expected<std::string, std::string> encode_command(std::span<std::string const> arguments)
{
    std::string text;
    for (auto const& argument : arguments)
    {
        if (argument.find_first_of(std::string_view("\n\r\0", 3)) != argument.npos)
            return std::unexpected("argument contains a line break or NUL");
        if (!text.empty())
            text += ' ';
        text += argument;
    }
    auto parsed = parse_command(text);
    if (!parsed)
        return std::unexpected(parsed.error());
    auto const& spec = *find_spec(trim(text));
    size_t words = 1 + std::count(spec.name.begin(), spec.name.end(), ' ');
    if (arguments.size() != words + spec.arguments)
        return std::unexpected("usage: " + std::string(spec.usage));
    return text;
}

} // namespace lwm::command
