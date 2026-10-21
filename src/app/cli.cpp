#include "cli.hpp"

#include <set>
#include <string_view>

namespace lwm::cli {

namespace {

// Sets an option to the choice its value spells.
template <typename T>
std::expected<void, std::string> choose(
    T& option, std::string_view value, std::string_view what,
    std::initializer_list<std::pair<std::string_view, std::type_identity_t<T>>> choices
)
{
    for (auto const& [spelling, choice] : choices)
        if (value == spelling)
            return option = choice, std::expected<void, std::string>{ };
    return std::unexpected("invalid log " + std::string(what) + " '" + std::string(value) + "'");
}

} // namespace

std::expected<Options, std::string> parse(int argc, char* const argv[])
{
    if (argc < 1 || !argv)
        return std::unexpected("invalid argument vector");

    Options options;
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
        if (arg == "--check-config")
        {
            options.check_config = true;
            continue;
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

        using enum quill::LogLevel;
        std::expected<void, std::string> chosen;
        if (name == "--config")
            options.config_path = std::string(value);
        else if (name == "--log-level")
            chosen = choose(options.log.level, value, "level", {
                { "trace", TraceL3 }, { "debug", Debug }, { "info", Info }, { "warn", Warning },
                { "error", Error }, { "critical", Critical }, { "off", None } });
        else if (name == "--log-target")
            chosen = choose(options.log.target, value, "target",
                            { { "journal", log::Target::Journal }, { "stderr", log::Target::Stderr } });
        else
            chosen = choose(options.log.color, value, "color", { { "auto", log::ColorMode::Automatic },
                            { "always", log::ColorMode::Always }, { "never", log::ColorMode::Never } });
        if (!chosen)
            return std::unexpected(chosen.error());
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
          "      --check-config         validate the configuration file and exit\n"
          "      --config PATH          select a configuration file\n"
          "      --log-level LEVEL      trace, debug, info, warn, error, critical, off\n"
          "      --log-target TARGET    journal (default) or stderr\n"
          "      --log-color MODE       auto, always, or never\n"
          "\n"
          "Value options accept --option VALUE or --option=VALUE, once each.\n";
}

} // namespace lwm::cli
