#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lwm {

struct CommandConfig
{
    enum class Kind
    {
        Shell,
        Argv
    };

    Kind kind = Kind::Shell;
    std::string shell;
    std::vector<std::string> argv;

    bool operator==(CommandConfig const&) const = default;

    static CommandConfig shell_command(std::string value)
    {
        CommandConfig command;
        command.kind = Kind::Shell;
        command.shell = std::move(value);
        return command;
    }

    static CommandConfig argv_command(std::vector<std::string> value)
    {
        CommandConfig command;
        command.kind = Kind::Argv;
        command.argv = std::move(value);
        return command;
    }

    bool empty() const
    {
        return kind == Kind::Shell ? shell.empty() : argv.empty();
    }
};

} // namespace lwm
