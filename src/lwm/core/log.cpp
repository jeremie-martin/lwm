#include "log.hpp"
#include "events.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <iterator>
#include <mutex>
#include <quill/Backend.h>
#include <quill/sinks/Sink.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace lwm::log {
namespace {
constexpr size_t record_limit = 4096;
struct Counters
{
    std::atomic<uint64_t> queue_drops{ }, delivery_drops{ }, truncations{ }, backend_notifications{ };
    std::atomic<int> last_error{ };
} counters;
Handle handle;
LogOptions options;
std::string instance;
std::string initialization_error;
// Only exceptional backend notifications and explicit status queries use this
// lock. Normal submissions and delivery counters never acquire it.
std::mutex notification_mutex;
std::string last_notification;

void delivery_failure(int error)
{
    counters.delivery_drops.fetch_add(1, std::memory_order_relaxed);
    counters.last_error.store(error, std::memory_order_relaxed);
}
void cap(std::string& text, size_t limit)
{
    if (text.size() <= limit)
        return;
    text.resize(limit - truncation_marker.size());
    text += truncation_marker;
    record_truncation();
}
// Binary native-journal fields cannot inject additional fields, even with embedded
// newlines or NULs. Lengths in this protocol are little endian.
void field(std::string& record, std::string_view name, std::string_view value, std::string_view suffix = { })
{
    record += name;
    record += '\n';
    uint64_t size = value.size() + suffix.size();
    for (int i = 0; i != 8; ++i) record += static_cast<char>(size >> (8 * i));
    record += value;
    record += suffix;
    record += '\n';
}

class Delivery final : public quill::Sink
{
public:
    ~Delivery() override { close(); }
    std::expected<void, std::string> open(LogOptions const& config)
    {
        close();
        target = config.target;
        if (target == Target::Journal)
        {
            char const* override = std::getenv("LWM_LOG_SOCKET");
            std::string_view path = override ? override : "/run/systemd/journal/socket";
            address = { };
            address.sun_family = AF_UNIX;
            if (path.empty() || path.front() != '/' || path.size() >= sizeof(address.sun_path))
                return std::unexpected("LWM_LOG_SOCKET must be an absolute Unix socket path shorter than 108 bytes");
            std::memcpy(address.sun_path, path.data(), path.size());
            fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        }
        else
        {
            struct stat st{ };
            if (::fstat(STDERR_FILENO, &st) != 0)
                return std::unexpected("cannot inspect stderr: " + std::string(std::strerror(errno)));
            struct stat null_st{ };
            bool null_device =
                ::stat("/dev/null", &null_st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == null_st.st_rdev;
            if (!S_ISFIFO(st.st_mode) && !::isatty(STDERR_FILENO) && !null_device)
                return std::unexpected(
                    "--log-target stderr requires a terminal, pipe, or /dev/null; use journal or pipe through tee for "
                    "files"
                );
            // dup() would share O_NONBLOCK with the launcher's descriptor.
            fd = ::open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
            color = config.color == ColorMode::Always || (config.color == ColorMode::Auto && ::isatty(fd));
        }
        if (fd < 0)
            return std::unexpected("cannot open log destination: " + std::string(std::strerror(errno)));
        return { };
    }
    void close()
    {
        if (!pending.empty())
        {
            delivery_failure(EAGAIN);
            pending.clear();
        }
        if (fd >= 0)
            ::close(fd);
        fd = -1;
    }

private:
    int fd = -1;
    Target target = Target::Journal;
    bool color = false;
    sockaddr_un address{ };
    std::string pending;
    std::string record;

    void drain()
    {
        if (pending.empty())
            return;
        ssize_t n = ::write(fd, pending.data(), pending.size());
        if (n > 0)
            pending.erase(0, static_cast<size_t>(n));
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            delivery_failure(errno);
            pending.clear();
        }
    }
    void write_log(
        quill::MacroMetadata const* metadata,
        uint64_t timestamp,
        std::string_view,
        std::string_view,
        std::string const&,
        std::string_view,
        quill::LogLevel level,
        std::string_view description,
        std::string_view,
        std::vector<std::pair<std::string, std::string>> const*,
        std::string_view message,
        std::string_view
    ) override
    {
        record.clear();
        record.reserve(record_limit);
        if (target == Target::Journal)
        {
            int priority = level >= quill::LogLevel::Critical ? 2
                : level >= quill::LogLevel::Error             ? 3
                : level >= quill::LogLevel::Warning           ? 4
                : level >= quill::LogLevel::Info              ? 6
                                                              : 7;
            fmtquill::format_to(
                std::back_inserter(record),
                "PRIORITY={}\nSYSLOG_IDENTIFIER=lwm\nLWM_LOG_INSTANCE={}\nLWM_TIMESTAMP_NS={}\n",
                priority,
                instance,
                timestamp
            );
            field(record, "CODE_FILE", metadata->full_path().substr(0, 512));
            field(record, "CODE_LINE", metadata->line());
            field(record, "CODE_FUNC", std::string_view(metadata->caller_function()).substr(0, 256));
            size_t available = record_limit - record.size() - 17; // MESSAGE + newline + u64 + newline
            if (message.size() > available)
            {
                field(record, "MESSAGE", message.substr(0, available - truncation_marker.size()), truncation_marker);
                record_truncation();
            }
            else
                field(record, "MESSAGE", message);
            iovec io{ record.data(), record.size() };
            msghdr msg{ };
            msg.msg_name = &address;
            msg.msg_namelen = sizeof(address);
            msg.msg_iov = &io;
            msg.msg_iovlen = 1;
            if (::sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
                delivery_failure(errno);
        }
        else
        {
            drain();
            if (!pending.empty())
            {
                delivery_failure(EAGAIN);
                return;
            }
            if (color)
                record = level >= quill::LogLevel::Warning ? "\033[31m" : "\033[36m";
            record += description;
            record += " [";
            record += metadata->file_name();
            record += ':';
            record += metadata->line();
            record += "] ";
            // Escape control bytes so a message cannot forge another terminal record.
            for (unsigned char c : message)
            {
                if (record.size() >= record_limit)
                    break;
                if (c < 32 || c == 127)
                {
                    constexpr char hex[] = "0123456789abcdef";
                    record += "\\x";
                    record += hex[c >> 4];
                    record += hex[c & 15];
                }
                else
                    record += static_cast<char>(c);
            }
            cap(record, record_limit - 6);
            if (color)
                record += "\033[0m";
            record += '\n';
            pending.swap(record);
            drain();
        }
    }
    void flush_sink() override { drain(); }
    void run_periodic_tasks() override { drain(); }
};
std::shared_ptr<Delivery> sink;
}

void record_truncation() noexcept { counters.truncations.fetch_add(1, std::memory_order_relaxed); }
void record_queue_drop() noexcept { counters.queue_drops.fetch_add(1, std::memory_order_relaxed); }
Handle* active_logger() noexcept { return &handle; }
std::expected<quill::LogLevel, std::string> parse_level(std::string_view value)
{
    using enum quill::LogLevel;
    for (auto level : { TraceL3, Debug, Info, Warning, Error, Critical, None })
        if (value == level_name(level))
            return level;
    if (value == "warning")
        return Warning;
    if (value == "err")
        return Error;
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
        return ColorMode::Auto;
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
    options = config;
    initialization_error.clear();
    try
    {
        if (instance.empty())
            instance = std::to_string(getpid()) + "-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        if (!sink)
            sink = std::static_pointer_cast<Delivery>(Frontend::create_or_get_sink<Delivery>("lwm-delivery"));
        if (auto result = sink->open(config); !result)
        {
            initialization_error = result.error();
            return result;
        }
        last_notification.reserve(argument_limit);
        quill::BackendOptions backend;
        // Native journal fields preserve arbitrary bytes; the stderr sink escapes
        // controls. Avoid scanning/rewriting every string twice in the worker.
        backend.check_printable_char = { };
        backend.backend_worker_on_poll_begin = []
        {
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
        // Ordinary diagnostics tolerate 100 ms delivery latency. Verbose modes
        // poll more often to keep their larger traffic below the queue bound.
        backend.sleep_duration = std::chrono::milliseconds(config.level >= quill::LogLevel::Info ? 100 : 10);
        backend.transit_event_buffer_initial_capacity = 256;
        backend.transit_events_soft_limit = 128;
        backend.transit_events_hard_limit = 256;
        backend.error_notifier = [](std::string const& message)
        {
            std::lock_guard lock(notification_mutex);
            last_notification.assign(message, 0, argument_limit);
            if (message.size() > argument_limit)
                last_notification
                    .replace(argument_limit - truncation_marker.size(), truncation_marker.size(), truncation_marker);
            counters.backend_notifications.fetch_add(1, std::memory_order_relaxed);
        };
        if (config.level != quill::LogLevel::None)
        {
            quill::Backend::start(backend);
            Frontend::preallocate();
        }
        auto* logger = Frontend::create_or_get_logger("lwm", sink, quill::PatternFormatterOptions{ "%(message)" });
        logger->set_log_level(config.level);
        options = config;
        handle.logger = logger;
        return { };
    }
    catch (std::exception const& e)
    {
        shutdown();
        initialization_error = e.what();
        return std::unexpected(initialization_error);
    }
}
void shutdown()
{
    handle.logger = nullptr;
    quill::Backend::stop(); // drains the finite queue; sink callbacks never wait for delivery
    if (sink)
        sink->close();
}
LogOptions prepare_exec()
{
    auto saved = options;
    shutdown();
    return saved;
}
std::expected<void, std::string> restore(LogOptions const& saved) { return initialize(saved); }
std::string status_json()
{
    std::string notification;
    {
        std::lock_guard lock(notification_mutex);
        notification = last_notification;
    }
    return fmtquill::format(
        "{{\"target\":\"{}\",\"level\":\"{}\",\"instance\":\"{}\",\"active\":{},\"queue_drops\":{},"
        "\"delivery_drops\":{},\"truncations\":{},\"backend_notifications\":{},\"last_delivery_error\":{},"
        "\"initialization_error\":\"{}\",\"last_backend_notification\":\"{}\"}}",
        options.target == Target::Journal ? "journal" : "stderr",
        level_name(options.level),
        instance,
        handle.logger != nullptr,
        counters.queue_drops.load(),
        counters.delivery_drops.load(),
        counters.truncations.load(),
        counters.backend_notifications.load(),
        counters.last_error.load(),
        json_escape(initialization_error),
        json_escape(notification)
    );
}
} // namespace lwm::log
