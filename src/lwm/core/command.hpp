#pragma once

#include "lwm/core/action.hpp"
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace lwm::command {

// Read-only requests answered from the current state.
enum class Query
{
    Version,
    WorkspaceList,
    WindowList,
    ScratchpadList,
    State,
};

// Mutating commands are ordinary WM actions, executed like key bindings.
using Request = std::variant<Query, Action>;

struct CommandSpec
{
    std::string_view name;
    std::string_view usage;
    std::string_view description;
    std::expected<Request, std::string> (*parse)(std::string_view, CommandSpec const&);
};

// The X-carried protocol: the WM_S0 owner window, named `identity`, takes requests
// in `command_atom`, answers in `reply_atom` and publishes `state_atom`.
inline constexpr std::string_view identity = "lwm";
inline constexpr char const *command_atom = "_LWM_COMMAND", *reply_atom = "_LWM_REPLY", *state_atom = "_LWM_STATE";
inline constexpr size_t max_request_bytes = 4096;
// A declared name must survive the longest name-bearing command unchanged.
bool valid_scratchpad_name(std::string_view name);
std::span<CommandSpec const> command_specs();
std::expected<Request, std::string> parse_command(std::string_view text);

} // namespace lwm::command
