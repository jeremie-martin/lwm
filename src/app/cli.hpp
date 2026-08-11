#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lwm/core/log.hpp"

namespace lwm::cli {

struct Options
{
    lwm::log::LogOptions log;
    std::optional<std::string> config_path;
    std::vector<std::string> restart_argv;
    bool help = false;
    bool version = false;
};

/// Parse LWM startup arguments without consulting the filesystem or X11.
std::expected<Options, std::string> parse(int argc, char* const argv[]);

/// Return the complete startup help text.
std::string usage(std::string_view program = "lwm");

} // namespace lwm::cli
