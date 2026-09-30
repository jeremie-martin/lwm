#include "ipc_command.hpp"
#include "events.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace lwm::ipc {
namespace {
using enum CommandId;
constexpr CommandSpec specs[] = {
    { LogStatus, "log status", Argument::None, "log status", "show logging delivery counters as JSON" },
    {            Ping,"ping",   Argument::None,"ping",     "check whether the WM is running"                                                             },
    {         Version,           "version",   Argument::None,                       "version",                     "show WM version" },
    {          Reload,     "reload-config",   Argument::None,                 "reload-config",                "reload configuration" },
    {         Restart,           "restart",   Argument::None,                       "restart",                      "restart the WM" },
    {            Exec,              "exec",   Argument::Text,                     "exec PATH",         "restart with another binary" },
    {          Layout,        "layout set",   Argument::Text,               "layout set NAME",      "select master-stack or monocle" },
    {        RatioSet,         "ratio set", Argument::Number,               "ratio set VALUE",            "set the root split ratio" },
    {      RatioReset,       "ratio reset",   Argument::None,                   "ratio reset",        "reset workspace split ratios" },
    {     RatioAdjust,      "ratio adjust", Argument::Number,            "ratio adjust DELTA",         "adjust the root split ratio" },
    {       Attention,  "notify-attention", Argument::Window, "notify-attention window=<xid>",         "mark an exact window urgent" },
    { WorkspaceSwitch,  "workspace switch",  Argument::Index,            "workspace switch N",       "switch workspace (zero-based)" },
    {   WorkspaceNext,    "workspace next",   Argument::None,                "workspace next",        "switch to the next workspace" },
    {   WorkspacePrev,    "workspace prev",   Argument::None,                "workspace prev",    "switch to the previous workspace" },
    {   WorkspaceList,    "workspace list",   Argument::None,                "workspace list",            "print workspaces as JSON" },
    {       FocusNext,        "focus next",   Argument::None,                    "focus next",           "focus the next MRU window" },
    {       FocusPrev,        "focus prev",   Argument::None,                    "focus prev",       "focus the previous MRU window" },
    {     FocusWindow,             "focus", Argument::Window,            "focus window=<xid>",               "focus an exact window" },
    {      WindowList,       "window list",   Argument::None,                   "window list",        "print normal clients as JSON" },
    {           Stash,  "scratchpad stash",   Argument::None,              "scratchpad stash",             "stash the active window" },
    {           Cycle,  "scratchpad cycle",   Argument::None,              "scratchpad cycle",           "cycle the scratchpad pool" },
    {          Toggle, "scratchpad toggle",   Argument::Text,        "scratchpad toggle NAME",           "toggle a named scratchpad" },
    {    CancelLaunch,
     "scratchpad cancel-launch",   Argument::Text,
     "scratchpad cancel-launch NAME",         "cancel pending launch state"                                                         },
    {  ScratchpadList,   "scratchpad list",   Argument::None,               "scratchpad list",           "print scratchpads as JSON" },
    {       Subscribe,         "subscribe", Argument::Filter,            "subscribe [FILTER]",         "stream filtered JSON events" },
    {           State,             "state",   Argument::None,                         "state", "print one consistent state snapshot" },
};
constexpr std::string_view spaces = " \t\r\n\v\f";
std::string_view trim(std::string_view text)
{
    auto first = text.find_first_not_of(spaces);
    if (first == text.npos)
        return {};
    return text.substr(first, text.find_last_not_of(spaces) - first + 1);
}
bool matches(std::string_view text, std::string_view name)
{
    return text == name || (text.starts_with(name) && text.size() > name.size() && spaces.contains(text[name.size()]));
}
}
std::span<CommandSpec const> command_specs() { return specs; }

std::expected<Command, std::string> parse_command(std::string_view text)
{
    if (text.size() + 1 >= max_request_bytes)
        return std::unexpected("request too large");
    text = trim(text);
    if (text.find_first_of(std::string_view("\n\r\0", 3)) != text.npos)
        return std::unexpected("command contains a line break or NUL");
    if (text.empty())
        return std::unexpected("empty command");
    CommandSpec const* spec = nullptr;
    for (auto const& candidate : specs)
        if (matches(text, candidate.name) && (!spec || candidate.name.size() > spec->name.size()))
            spec = &candidate;
    if (!spec)
        return std::unexpected("unknown command");
    auto value = trim(text.substr(spec->name.size()));
    Command command{ spec->id, {} };
    auto usage = [&] { return std::unexpected("usage: " + std::string(spec->usage)); };
    switch (spec->argument)
    {
        case Argument::None:
            if (!value.empty())
                return usage();
            break;
        case Argument::Text:
            if (value.empty())
                return usage();
            if (spec->id == Layout && value != "master-stack" && value != "monocle")
                return std::unexpected("unknown layout: " + std::string(value));
            command.argument = std::string(value);
            break;
        case Argument::Filter:
        {
            auto mask = parse_event_filter(value);
            if (!mask)
                return std::unexpected("no recognized event types in filter");
            command.argument = mask;
            break;
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
            if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size()
                || !std::isfinite(number))
                return std::unexpected(
                    std::string(spec->id == RatioSet ? "invalid ratio value: " : "invalid delta value: ")
                    + std::string(value)
                );
            command.argument = number;
            break;
        }
        case Argument::Window:
        case Argument::Index:
        {
            if (value.empty() || value.find_first_of(spaces) != value.npos)
                return usage();
            if (spec->argument == Argument::Window)
            {
                if (!value.starts_with("window="))
                    return usage();
                value.remove_prefix(7);
            }
            auto digits = value;
            int base = 10;
            if (spec->argument == Argument::Window && (digits.starts_with("0x") || digits.starts_with("0X")))
            {
                digits.remove_prefix(2);
                base = 16;
            }
            if (digits.empty())
                return std::unexpected("invalid window id: " + std::string(value));
            uint32_t number = 0;
            auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number, base);
            if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size())
                return std::unexpected(
                    std::string(
                        spec->argument == Argument::Window ? "invalid window id: " : "invalid workspace index: "
                    )
                    + std::string(value)
                );
            command.argument = number;
            break;
        }
    }
    return command;
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
    auto const& spec =
        *std::find_if(std::begin(specs), std::end(specs), [&](auto const& s) { return s.id == parsed->id; });
    size_t words = 1 + std::count(spec.name.begin(), spec.name.end(), ' ');
    if (spec.argument != Argument::Filter && arguments.size() != words + (spec.argument != Argument::None))
        return std::unexpected("usage: " + std::string(spec.usage));
    return text;
}
} // namespace lwm::ipc
