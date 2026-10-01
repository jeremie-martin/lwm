#include "cli.hpp"

#include <set>
#include <string_view>

namespace lwm::cli {

std::expected<Options, std::string> parse(int argc, char* const argv[])
{
    if (argc < 1 || !argv)
        return std::unexpected("invalid argument vector");

    Options options;
    options.restart_argv.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
        options.restart_argv.emplace_back(argv[i] ? argv[i] : "");

    std::set<std::string_view> seen;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i] ? argv[i] : "";
        if (arg == "-h" || arg == "--help")
        {
            options.help = true;
            return options;
        }
        if (arg == "-v" || arg == "--version")
        {
            options.version = true;
            return options;
        }

        auto separator = arg.find('=');
        auto name = arg.substr(0, separator);
        if (name != "--config" && name != "--log-level" && name != "--log-target" && name != "--log-color")
            return std::unexpected("unknown argument: " + std::string(arg));
        if (!seen.insert(name).second)
            return std::unexpected("duplicate option: " + std::string(name));

        std::string_view value;
        if (separator != arg.npos)
            value = arg.substr(separator + 1);
        else
        {
            if (++i >= argc)
                return std::unexpected("missing value for " + std::string(name));
            value = argv[i] ? argv[i] : "";
        }
        if (value.empty())
            return std::unexpected("empty value for " + std::string(name));

        if (name == "--config")
            options.config_path = std::string(value);
        else if (name == "--log-level")
        {
            auto level = log::parse_level(value);
            if (!level)
                return std::unexpected(level.error());
            options.log.level = *level;
        }
        else if (name == "--log-target")
        {
            auto target = log::parse_target(value);
            if (!target)
                return std::unexpected(target.error());
            options.log.target = *target;
        }
        else if (name == "--log-color")
        {
            auto color = log::parse_color_mode(value);
            if (!color)
                return std::unexpected(color.error());
            options.log.color = *color;
        }
    }
    return options;
}

std::string usage(std::string_view program)
{
    return "usage: " + std::string(program)
        + " [OPTIONS]\n"
          "\n"
          "startup options:\n"
          "  -h, --help                 show this help\n"
          "  -v, --version              show the installed version\n"
          "      --config PATH          select a configuration file\n"
          "      --log-level LEVEL      trace, debug, info, warn, error, critical, off\n"
          "      --log-target TARGET    journal (default) or stderr\n"
          "      --log-color MODE       auto, always, or never\n"
          "\n"
          "Value options accept --option VALUE or --option=VALUE, once each.\n";
}

} // namespace lwm::cli
