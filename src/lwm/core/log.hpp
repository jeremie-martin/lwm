#pragma once

// Logging for LWM using spdlog
//
// Log levels (compile-time filtered via SPDLOG_ACTIVE_LEVEL):
//   - TRACE: Very verbose, per-event logging (e.g., every X11 event)
//   - DEBUG: Detailed debugging info (e.g., state changes, decisions)
//   - INFO:  Normal operational messages (e.g., startup, config loaded)
//   - WARN:  Warning conditions
//   - ERROR: Error conditions
//
// In Release builds: TRACE and DEBUG are compiled out (zero cost)
// In Debug builds: All levels are active
//
// Usage:
//   LOG_DEBUG("Processing window {:#x}", window_id);
//   LOG_INFO("Workspace switched to {}", workspace);

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace lwm::log {

// Initialize logging - call once at startup
inline void init()
{
    // Create console sink (stderr) with colors
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console_sink->set_level(spdlog::level::trace);
    console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    // Create file sink for persistent logs
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("/tmp/lwm.log", true);
    file_sink->set_level(spdlog::level::trace);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");

    // Create logger with both sinks
    auto logger = std::make_shared<spdlog::logger>("lwm", spdlog::sinks_init_list{console_sink, file_sink});
    logger->set_level(spdlog::level::trace);  // Runtime level (compile-time is separate)
    logger->flush_on(spdlog::level::debug);   // Flush on debug and above

    // Set as default logger
    spdlog::set_default_logger(logger);
}

// Shutdown logging - call at exit
inline void shutdown()
{
    spdlog::shutdown();
}

} // namespace lwm::log

// Convenience macros using spdlog's compile-time filtered macros
// These are zero-cost when level is below SPDLOG_ACTIVE_LEVEL

#define LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)  SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

// Key event logging helper (trace level)
#define LOG_KEY(state, keysym) \
    SPDLOG_TRACE("Key: state={:#x} keysym={:#x}", state, keysym)
