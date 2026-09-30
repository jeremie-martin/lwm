#pragma once

#include <cstring>
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

// Encode strings directly into Quill's queue, with a marker only when truncated.
// The backend decodes the ordinary string-view wire format; it never borrows caller memory.
struct BoundedString
{
    std::string_view value;
    bool truncated;
};
inline constexpr size_t argument_limit = 1024;
inline constexpr std::string_view truncation_marker = "...[truncated]";
void record_truncation() noexcept;
void record_queue_drop() noexcept;
inline BoundedString bound(std::string_view value)
{
    bool truncated = value.size() > argument_limit;
    if (truncated)
    {
        record_truncation();
        value = value.substr(0, argument_limit - truncation_marker.size());
    }
    return { value, truncated };
}
template <typename T> decltype(auto) bounded(T&& value)
{
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>)
        return bound(value);
    else if constexpr (std::is_convertible_v<T, char const*>)
    {
        char const* text = value;
        return bound(
            text ? std::string_view(
                       text,
                       strnlen(
                           text,
                           std::is_array_v<U> ? std::min(std::extent_v<U>, argument_limit + 1) : argument_limit + 1
                       )
                   )
                 : std::string_view("(null)")
        );
    }
    else
        return std::forward<T>(value);
}

// All submissions and lifecycle calls belong to the WM thread. Only sink counters
// are shared with the worker. Quill's macros keep metadata and disabled-argument gates.
struct Handle
{
    Logger* logger = nullptr;
    template <quill::LogLevel level> bool should_log_statement() const noexcept
    {
        return logger && logger->template should_log_statement<level>();
    }
    template <bool immediate, typename... Args> void log_statement(quill::MacroMetadata const* metadata, Args&&... args)
    {
        static_assert(!immediate);
        if (!logger->template log_statement<false>(metadata, bounded(std::forward<Args>(args))...))
            record_queue_drop();
    }
};
Handle* active_logger() noexcept;
std::expected<quill::LogLevel, std::string> parse_level(std::string_view);
std::expected<ColorMode, std::string> parse_color_mode(std::string_view);
std::expected<Target, std::string> parse_target(std::string_view);
std::string level_name(quill::LogLevel);
std::expected<void, std::string> initialize(LogOptions options = { });
LogOptions prepare_exec();
std::expected<void, std::string> restore(LogOptions const&);
void shutdown();
std::string status_json();
} // namespace lwm::log

namespace quill {
template <> struct Codec<lwm::log::BoundedString> : Codec<std::string_view>
{
    static size_t compute_encoded_size(detail::SizeCacheVector&, lwm::log::BoundedString const& arg) noexcept
    {
        return sizeof(uint32_t) + arg.value.size() + (arg.truncated ? lwm::log::truncation_marker.size() : 0);
    }
    static void
    encode(std::byte*& buffer, detail::SizeCacheVector const&, uint32_t&, lwm::log::BoundedString const& arg) noexcept
    {
        uint32_t size = arg.value.size() + (arg.truncated ? lwm::log::truncation_marker.size() : 0);
        std::memcpy(buffer, &size, sizeof(size));
        buffer += sizeof(size);
        if (!arg.value.empty())
            std::memcpy(buffer, arg.value.data(), arg.value.size());
        buffer += arg.value.size();
        if (arg.truncated)
        {
            std::memcpy(buffer, lwm::log::truncation_marker.data(), lwm::log::truncation_marker.size());
            buffer += lwm::log::truncation_marker.size();
        }
    }
};
}
#define LWM_LOG_TRACE(...) QUILL_LOG_TRACE_L3(::lwm::log::active_logger(), __VA_ARGS__)
#define LWM_LOG_DEBUG(...) QUILL_LOG_DEBUG(::lwm::log::active_logger(), __VA_ARGS__)
#define LWM_LOG_INFO(...) QUILL_LOG_INFO(::lwm::log::active_logger(), __VA_ARGS__)
#define LWM_LOG_WARN(...) QUILL_LOG_WARNING(::lwm::log::active_logger(), __VA_ARGS__)
#define LWM_LOG_ERROR(...) QUILL_LOG_ERROR(::lwm::log::active_logger(), __VA_ARGS__)
#define LWM_LOG_CRITICAL(...) QUILL_LOG_CRITICAL(::lwm::log::active_logger(), __VA_ARGS__)
#define LWM_LOG_WARN_LIMIT(interval, ...) QUILL_LOG_WARNING_LIMIT(interval, ::lwm::log::active_logger(), __VA_ARGS__)
#define LWM_LOG_KEY(state, keysym) LWM_LOG_TRACE("Key: state={:#x} keysym={:#x}", state, keysym)
