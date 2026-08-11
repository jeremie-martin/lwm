#include "cli.hpp"

#include <string_view>

namespace lwm::cli {
namespace {

std::expected<std::string, std::string> argument_value(int& index, int argc, char* const argv[], std::string_view option)
{
    if (index + 1 >= argc)
        return std::unexpected("missing value for " + std::string(option));
    ++index;
    std::string value = argv[index] ? argv[index] : "";
    if (value.empty())
        return std::unexpected("empty value for " + std::string(option));
    return value;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.starts_with(prefix);
}

std::expected<void, std::string> reject_duplicate(bool& seen, std::string_view option)
{
    if (seen)
        return std::unexpected("duplicate option: " + std::string(option));
    seen = true;
    return {};
}

} // namespace

std::expected<Options, std::string> parse(int argc, char* const argv[])
{
    if (argc < 1 || !argv)
        return std::unexpected("invalid argument vector");

    Options options;
    options.restart_argv.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
        options.restart_argv.emplace_back(argv[i] ? argv[i] : "");

    bool seen_config = false;
    bool seen_level = false;
    bool seen_verbose = false;
    bool seen_debug = false;
    bool seen_file = false;
    bool seen_no_file = false;
    bool seen_color = false;
    bool positional_allowed = true;

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i] ? argv[i] : "";
        if (positional_allowed && arg == "--")
        {
            positional_allowed = false;
            continue;
        }

        auto set_config = [&](std::string value) -> std::expected<void, std::string> {
            if (value.empty())
                return std::unexpected("empty value for --config");
            if (auto result = reject_duplicate(seen_config, "--config"); !result)
                return std::unexpected(result.error());
            if (options.config_path)
                return std::unexpected("duplicate config path");
            options.config_path = std::move(value);
            return {};
        };

        // Once the terminator is consumed, every remaining token is a literal
        // config path, including strings that look like known options.
        if (!positional_allowed)
        {
            if (arg.empty())
                return std::unexpected("empty config path");
            if (options.config_path)
                return std::unexpected("duplicate config path");
            options.config_path = std::string(arg);
            continue;
        }

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

        if (arg == "-V" || arg == "--verbose")
        {
            if (auto result = reject_duplicate(seen_verbose, "--verbose"); !result)
                return std::unexpected(result.error());
            options.log.level = spdlog::level::debug;
            continue;
        }

        if (arg == "--debug")
        {
            if (auto result = reject_duplicate(seen_debug, "--debug"); !result)
                return std::unexpected(result.error());
            options.log.level = spdlog::level::trace;
            continue;
        }

        if (arg == "-d" || (arg.size() > 2 && arg.starts_with("-d")))
        {
            if (auto result = reject_duplicate(seen_debug, "-d"); !result)
                return std::unexpected(result.error());
            std::string value;
            if (arg == "-d")
            {
                auto parsed = argument_value(i, argc, argv, "-d");
                if (!parsed)
                    return std::unexpected(parsed.error());
                value = std::move(*parsed);
            }
            else
                value = std::string(arg.substr(2));
            if (value != "all")
                return std::unexpected("invalid -d value '" + value + "' (expected all)");
            options.log.level = spdlog::level::trace;
            continue;
        }

        if (arg == "--no-log-file")
        {
            if (auto result = reject_duplicate(seen_no_file, "--no-log-file"); !result)
                return std::unexpected(result.error());
            if (seen_file)
                return std::unexpected("--no-log-file conflicts with --log-file");
            options.log.no_log_file = true;
            continue;
        }

        if (arg == "--log-level" || starts_with(arg, "--log-level="))
        {
            if (auto result = reject_duplicate(seen_level, "--log-level"); !result)
                return std::unexpected(result.error());
            std::string value;
            if (arg == "--log-level")
            {
                auto parsed = argument_value(i, argc, argv, "--log-level");
                if (!parsed)
                    return std::unexpected(parsed.error());
                value = std::move(*parsed);
            }
            else
                value = std::string(arg.substr(std::string_view("--log-level=").size()));
            auto level = lwm::log::parse_level(value);
            if (!level)
                return std::unexpected(level.error());
            options.log.level = *level;
            continue;
        }

        if (arg == "--log-file" || starts_with(arg, "--log-file="))
        {
            if (auto result = reject_duplicate(seen_file, "--log-file"); !result)
                return std::unexpected(result.error());
            if (seen_no_file)
                return std::unexpected("--log-file conflicts with --no-log-file");
            std::string value;
            if (arg == "--log-file")
            {
                auto parsed = argument_value(i, argc, argv, "--log-file");
                if (!parsed)
                    return std::unexpected(parsed.error());
                value = std::move(*parsed);
            }
            else
                value = std::string(arg.substr(std::string_view("--log-file=").size()));
            if (value.empty())
                return std::unexpected("empty value for --log-file");
            options.log.log_file = value;
            continue;
        }

        if (arg == "--log-color" || starts_with(arg, "--log-color="))
        {
            if (auto result = reject_duplicate(seen_color, "--log-color"); !result)
                return std::unexpected(result.error());
            std::string value;
            if (arg == "--log-color")
            {
                auto parsed = argument_value(i, argc, argv, "--log-color");
                if (!parsed)
                    return std::unexpected(parsed.error());
                value = std::move(*parsed);
            }
            else
                value = std::string(arg.substr(std::string_view("--log-color=").size()));
            auto color = lwm::log::parse_color_mode(value);
            if (!color)
                return std::unexpected(color.error());
            options.log.color = *color;
            continue;
        }

        if (arg == "-c" || (arg.size() > 2 && arg.starts_with("-c")) || arg == "--config" || starts_with(arg, "--config="))
        {
            std::string value;
            if (arg == "-c")
            {
                auto parsed = argument_value(i, argc, argv, "--config");
                if (!parsed)
                    return std::unexpected(parsed.error());
                value = std::move(*parsed);
            }
            else if (arg.size() > 2 && arg.starts_with("-c"))
                value = std::string(arg.substr(2));
            else if (arg == "--config")
            {
                auto parsed = argument_value(i, argc, argv, "--config");
                if (!parsed)
                    return std::unexpected(parsed.error());
                value = std::move(*parsed);
            }
            else
                value = std::string(arg.substr(std::string_view("--config=").size()));
            auto result = set_config(std::move(value));
            if (!result)
                return std::unexpected(result.error());
            continue;
        }

        if (!arg.empty() && arg.front() == '-' && positional_allowed)
            return std::unexpected("unknown option: " + std::string(arg));

        if (arg.empty())
            return std::unexpected("empty config path");
        if (options.config_path)
            return std::unexpected("duplicate config path");
        options.config_path = std::string(arg);
    }

    return options;
}

std::string usage(std::string_view program)
{
    return "usage: " + std::string(program) + " [OPTIONS] [CONFIG]\n"
           "\n"
           "startup options:\n"
           "  -h, --help                 show this help\n"
           "  -v, --version              show the installed version\n"
           "  -V, --verbose              enable DEBUG logging\n"
           "  -d all, --debug            enable TRACE logging\n"
           "      --log-level LEVEL      trace, debug, info, warn, error, critical\n"
           "      --log-file PATH        write WARN-and-higher records to PATH\n"
           "      --no-log-file           disable the private error log\n"
           "      --log-color MODE       auto, always, or never\n"
           "  -c, --config PATH          select a configuration file\n"
           "\n"
           "A single bare CONFIG path is retained for compatibility. Use -- to\n"
           "terminate option parsing when the path begins with '-'.\n";
}

} // namespace lwm::cli
