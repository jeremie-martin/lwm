#pragma once

#include <sstream>
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

    std::string describe() const
    {
        if (kind == Kind::Shell)
            return shell;

        std::ostringstream out;
        for (size_t i = 0; i < argv.size(); ++i)
        {
            if (i != 0)
                out << ' ';
            out << argv[i];
        }
        return out.str();
    }
};

} // namespace lwm
