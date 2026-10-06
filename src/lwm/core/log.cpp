#include "log.hpp"
#include <chrono>
#include <csignal>
#include <quill/Backend.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/SystemdSink.h>
#include <unistd.h>

namespace lwm::log {
namespace {
Logger* logger = nullptr;
}
Logger* active_logger() noexcept { return logger; }
std::expected<quill::LogLevel, std::string> parse_level(std::string_view value)
{
    using enum quill::LogLevel;
    for (auto level : { TraceL3, Debug, Info, Warning, Error, Critical, None })
        if (value == level_name(level))
            return level;
    return std::unexpected("invalid log level '" + std::string(value) + "'");
}
std::string level_name(quill::LogLevel level)
{
    using enum quill::LogLevel;
    switch (level)
    {
        case TraceL3:
            return "trace";
        case Debug:
            return "debug";
        case Info:
            return "info";
        case Warning:
            return "warn";
        case Error:
            return "error";
        case Critical:
            return "critical";
        default:
            return "off";
    }
}
std::expected<ColorMode, std::string> parse_color_mode(std::string_view value)
{
    if (value == "auto")
        return ColorMode::Automatic;
    if (value == "always")
        return ColorMode::Always;
    if (value == "never")
        return ColorMode::Never;
    return std::unexpected("invalid log color '" + std::string(value) + "'");
}
std::expected<Target, std::string> parse_target(std::string_view value)
{
    if (value == "journal")
        return Target::Journal;
    if (value == "stderr")
        return Target::Stderr;
    return std::unexpected("invalid log target '" + std::string(value) + "' (expected journal or stderr)");
}
std::expected<void, std::string> initialize(LogOptions config)
{
    shutdown();
    try
    {
        if (config.level == quill::LogLevel::None)
            return { };

        std::shared_ptr<quill::Sink> sink;
        if (config.target == Target::Journal)
            sink = Frontend::create_or_get_sink<quill::SystemdSink>("journal");
        else
        {
            quill::ConsoleSinkConfig console;
            console.set_stream("stderr");
            console.set_colour_mode(config.color);
            sink = Frontend::create_or_get_sink<quill::ConsoleSink>("stderr", console);
        }
        quill::BackendOptions backend;
        // Ordinary diagnostics tolerate batching; verbose modes drain more often.
        backend.sleep_duration = std::chrono::milliseconds(config.level >= quill::LogLevel::Info ? 100 : 10);
        backend.transit_event_buffer_initial_capacity = 256;
        backend.transit_events_soft_limit = 128;
        backend.transit_events_hard_limit = 256;
        backend.backend_worker_on_poll_begin = []
        {
            // A closed diagnostic pipe must not kill the WM. Do not change the
            // signal disposition inherited by applications launched from it.
            static thread_local bool configured = false;
            if (!configured)
            {
                sigset_t mask;
                sigemptyset(&mask);
                sigaddset(&mask, SIGPIPE);
                pthread_sigmask(SIG_BLOCK, &mask, nullptr);
                configured = true;
            }
        };
        // Backend errors and overflow summaries are dropped: reporting them could
        // block on the stalled destination they describe.
        backend.error_notifier = [](std::string const&) { };
        quill::Backend::start(backend);
        Frontend::preallocate();
        logger = Frontend::create_or_get_logger(
            "lwm",
            sink,
            quill::PatternFormatterOptions{ config.target == Target::Journal
                                                ? "%(message)"
                                                : "%(time) %(log_level) [%(short_source_location)] %(message)" }
        );
        logger->set_log_level(config.level);
        return { };
    }
    catch (std::exception const& e)
    {
        shutdown();
        return std::unexpected(std::string(e.what()));
    }
}
void shutdown()
{
    logger = nullptr;
    // Standard sinks may wait for the destination. The WM thread stops submitting
    // before joining the worker; normal event handling never flushes or waits.
    quill::Backend::stop();
}
} // namespace lwm::log
