#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace lwm::ipc {
enum class CommandId
{
    Ping,
    Version,
    Reload,
    Restart,
    Exec,
    Layout,
    RatioSet,
    RatioReset,
    RatioAdjust,
    Attention,
    WorkspaceSwitch,
    WorkspaceNext,
    WorkspacePrev,
    WorkspaceList,
    FocusNext,
    FocusPrev,
    FocusWindow,
    WindowList,
    Stash,
    Cycle,
    Toggle,
    CancelLaunch,
    ScratchpadList,
    Subscribe,
    State
};
enum class Argument
{
    None,
    Text,
    Number,
    Index,
    Window,
    Filter
};
struct CommandSpec
{
    CommandId id;
    std::string_view name;
    Argument argument;
    std::string_view usage;
    std::string_view description;
};
struct Command
{
    CommandId id;
    std::variant<std::monostate, std::string, double, uint32_t> argument;
};
inline constexpr size_t max_request_bytes = 4096;
inline constexpr size_t max_reply_bytes = 8 * 1024 * 1024;
inline constexpr size_t max_event_bytes = 1024 * 1024;
std::span<CommandSpec const> command_specs();
std::expected<Command, std::string> parse_command(std::string_view text);
std::expected<std::string, std::string> encode_command(std::span<std::string const> arguments);
} // namespace lwm::ipc
