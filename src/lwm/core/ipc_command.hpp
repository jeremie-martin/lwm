#pragma once

#include "lwm/core/action.hpp"
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace lwm::ipc {

// Read-only requests answered from the current state.
enum class Query
{
    Ping,
    Version,
    LogStatus,
    WorkspaceList,
    WindowList,
    ScratchpadList,
    State,
};

struct Subscribe
{
    uint32_t mask = 0;
    bool operator==(Subscribe const&) const = default;
};

// Mutating commands are ordinary WM actions, executed like key bindings.
using Request = std::variant<Query, Subscribe, Action>;

enum class Argument
{
    None,
    Text,
    Number,
    Index,
    Window,
    Filter,
    Direction,
    Layout,
};

using Value = std::variant<std::monostate, std::string, double, uint32_t, int, LayoutStrategy>;

struct CommandSpec
{
    std::string_view name;
    Argument argument;
    std::string_view usage;
    std::string_view description;
    Request (*make)(Value const&);
};

inline constexpr size_t max_request_bytes = 4096;
inline constexpr size_t max_reply_bytes = 8 * 1024 * 1024;
inline constexpr size_t max_event_bytes = 1024 * 1024;
std::span<CommandSpec const> command_specs();
std::expected<Request, std::string> parse_command(std::string_view text);
std::expected<std::string, std::string> encode_command(std::span<std::string const> arguments);

} // namespace lwm::ipc
