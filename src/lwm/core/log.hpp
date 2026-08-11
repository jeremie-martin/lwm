#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

namespace lwm::log {

enum class ColorMode
{
    Auto,
    Always,
    Never
};

struct LogOptions
{
    spdlog::level::level_enum level = spdlog::level::info;
    std::optional<std::filesystem::path> log_file;
    bool no_log_file = false;
    ColorMode color = ColorMode::Auto;
    // Filled by initialize/current_options for the selected default or explicit file.
    std::optional<std::filesystem::path> resolved_file_path;
};

std::expected<spdlog::level::level_enum, std::string> parse_level(std::string_view value);
std::expected<ColorMode, std::string> parse_color_mode(std::string_view value);
std::string level_name(spdlog::level::level_enum level);

/// Return the currently owned logger. The pointer is never null.
std::shared_ptr<spdlog::logger> active_logger();

/// Build a complete logger and atomically make it active.
std::expected<void, std::string> initialize(LogOptions options = {});

/// Compatibility entry point for code that does not need startup diagnostics.
void init();

/// Return the options used by the active logger.
LogOptions current_options();

/// Flush all sinks owned by the active logger.
void flush();

/// Flush, then replace the active logger with the stderr fallback before exec/fork.
/// The returned options can be passed to restore after a failed parent exec.
LogOptions prepare_exec();

/// Restore a logger after a failed exec. Errors leave the fallback active.
std::expected<void, std::string> restore(LogOptions const& options);

/// Return to the stderr fallback without touching spdlog's global registry.
void shutdown();

} // namespace lwm::log

// These wrappers intentionally do not use spdlog's global/default logger macros.
// Keeping the source location in the macro preserves the caller's file and line.
#define LWM_LOG_AT(_level, ...) \
    do \
    { \
        auto _lwm_logger = ::lwm::log::active_logger(); \
        SPDLOG_LOGGER_CALL(_lwm_logger, (_level), __VA_ARGS__); \
    } while (false)

#define LOG_TRACE(...) LWM_LOG_AT(::spdlog::level::trace, __VA_ARGS__)
#define LOG_DEBUG(...) LWM_LOG_AT(::spdlog::level::debug, __VA_ARGS__)
#define LOG_INFO(...) LWM_LOG_AT(::spdlog::level::info, __VA_ARGS__)
#define LOG_WARN(...) LWM_LOG_AT(::spdlog::level::warn, __VA_ARGS__)
#define LOG_ERROR(...) LWM_LOG_AT(::spdlog::level::err, __VA_ARGS__)
#define LOG_CRITICAL(...) LWM_LOG_AT(::spdlog::level::critical, __VA_ARGS__)
#define LOG_KEY(state, keysym) LOG_TRACE("Key: state={:#x} keysym={:#x}", state, keysym)
