#pragma once

#include <expected>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <string>
#include <string_view>

namespace lwm::log {
enum class ColorMode
{
    Auto,
    Always,
    Never
};
enum class Target
{
    Journal,
    Stderr
};
struct LogOptions
{
    quill::LogLevel level = quill::LogLevel::Info;
    Target target = Target::Journal;
    ColorMode color = ColorMode::Auto;
};
struct FrontOptions : quill::FrontendOptions
{
    static constexpr auto queue_type = quill::QueueType::BoundedDropping;
    static constexpr size_t initial_queue_capacity = 256 * 1024;
};
using Frontend = quill::FrontendImpl<FrontOptions>;
using Logger = Frontend::logger_t;

// Submission and lifecycle calls belong to the WM thread.
Logger* active_logger() noexcept;
std::expected<quill::LogLevel, std::string> parse_level(std::string_view);
std::expected<ColorMode, std::string> parse_color_mode(std::string_view);
std::expected<Target, std::string> parse_target(std::string_view);
std::string level_name(quill::LogLevel);
std::expected<void, std::string> initialize(LogOptions options = { });
void shutdown();
std::string status_json();
} // namespace lwm::log

// The null guard covers startup, shutdown and failed initialization. Quill owns
// level gating, argument copying, source metadata and rate limiting.
#define LWM_LOG_CALL(macro, ...)                            \
    do                                                      \
    {                                                       \
        if (auto* lwm_logger = ::lwm::log::active_logger()) \
            macro(lwm_logger, __VA_ARGS__);                 \
    } while (false)
#define LWM_LOG_TRACE(...) LWM_LOG_CALL(QUILL_LOG_TRACE_L3, __VA_ARGS__)
#define LWM_LOG_DEBUG(...) LWM_LOG_CALL(QUILL_LOG_DEBUG, __VA_ARGS__)
#define LWM_LOG_INFO(...) LWM_LOG_CALL(QUILL_LOG_INFO, __VA_ARGS__)
#define LWM_LOG_WARN(...) LWM_LOG_CALL(QUILL_LOG_WARNING, __VA_ARGS__)
#define LWM_LOG_ERROR(...) LWM_LOG_CALL(QUILL_LOG_ERROR, __VA_ARGS__)
#define LWM_LOG_CRITICAL(...) LWM_LOG_CALL(QUILL_LOG_CRITICAL, __VA_ARGS__)
#define LWM_LOG_WARN_LIMIT(interval, ...)                               \
    do                                                                  \
    {                                                                   \
        if (auto* lwm_logger = ::lwm::log::active_logger())             \
            QUILL_LOG_WARNING_LIMIT(interval, lwm_logger, __VA_ARGS__); \
    } while (false)
#define LWM_LOG_KEY(state, keysym) LWM_LOG_TRACE("Key: state={:#x} keysym={:#x}", state, keysym)
