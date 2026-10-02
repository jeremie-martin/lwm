#pragma once

#include "lwm/core/action.hpp"
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace lwm::command {

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

struct CommandSpec
{
    std::string_view name;
    std::optional<size_t> arguments; ///< Fixed CLI argument count, or an arbitrary subscription filter list
    std::string_view usage;
    std::string_view description;
    std::expected<Request, std::string> (*parse)(std::string_view, CommandSpec const&);
};

inline constexpr size_t max_request_bytes = 4096;
std::span<CommandSpec const> command_specs();
std::expected<Request, std::string> parse_command(std::string_view text);
std::expected<std::string, std::string> encode_command(std::span<std::string const> arguments);

} // namespace lwm::command
