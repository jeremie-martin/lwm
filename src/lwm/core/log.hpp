#pragma once

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace lwm::log {

// Log levels: TRACE (very verbose), DEBUG (detailed), INFO (operational), WARN (warnings), ERROR (errors)
// Release builds: TRACE/DEBUG compiled out (zero cost)

inline void init()
{
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console_sink->set_level(spdlog::level::trace);
    console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("/tmp/lwm.log", true);
    file_sink->set_level(spdlog::level::trace);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");

    auto logger = std::make_shared<spdlog::logger>("lwm", spdlog::sinks_init_list{ console_sink, file_sink });
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::debug);

    spdlog::set_default_logger(logger);
}

inline void shutdown() { spdlog::shutdown(); }

} // namespace lwm::log

#define LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
#define LOG_KEY(state, keysym) SPDLOG_TRACE("Key: state={:#x} keysym={:#x}", state, keysym)
