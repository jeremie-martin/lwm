#include "lwm/core/log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace fs = std::filesystem;

namespace lwm::log {
namespace {

constexpr size_t MAX_LOG_SIZE = 1024U * 1024U;
constexpr size_t MAX_LOG_FILES = 3U;

std::mutex state_mutex;
std::shared_ptr<spdlog::logger> logger_state;
LogOptions options_state;

std::string category_for(char const* filename)
{
    if (!filename || !*filename)
        return "unknown";
    fs::path path(filename);
    std::string category = path.stem().string();
    if (category.empty())
        category = path.filename().string();
    return category.empty() ? "unknown" : category;
}

class category_flag final : public spdlog::custom_flag_formatter
{
public:
    void format(spdlog::details::log_msg const& message, std::tm const&, spdlog::memory_buf_t& destination) override
    {
        std::string category = category_for(message.source.filename);
        destination.append(category.data(), category.data() + category.size());
    }

    std::unique_ptr<spdlog::custom_flag_formatter> clone() const override
    {
        return std::make_unique<category_flag>();
    }
};

class level_flag final : public spdlog::custom_flag_formatter
{
public:
    void format(spdlog::details::log_msg const& message, std::tm const&, spdlog::memory_buf_t& destination) override
    {
        std::string name = level_name(message.level);
        destination.append(name.data(), name.data() + name.size());
    }

    std::unique_ptr<spdlog::custom_flag_formatter> clone() const override
    {
        return std::make_unique<level_flag>();
    }
};

std::unique_ptr<spdlog::formatter> make_formatter(std::string pattern)
{
    spdlog::pattern_formatter::custom_flags flags;
    flags['&'] = std::make_unique<category_flag>();
    flags['K'] = std::make_unique<level_flag>();
    return std::make_unique<spdlog::pattern_formatter>(std::move(pattern), spdlog::pattern_time_type::local, "\n", std::move(flags));
}

std::shared_ptr<spdlog::logger> make_fallback(spdlog::level::level_enum level)
{
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>(spdlog::color_mode::never);
    sink->set_level(level);
    sink->set_formatter(make_formatter("[%Y-%m-%dT%H:%M:%S.%e] [%^%K%$] [%&] %v"));
    auto logger = std::make_shared<spdlog::logger>("lwm", sink);
    logger->set_level(level);
    logger->flush_on(spdlog::level::warn);
    return logger;
}

std::shared_ptr<spdlog::logger> fallback_logger(spdlog::level::level_enum level = spdlog::level::info)
{
    if (level == spdlog::level::info)
    {
        static auto logger = make_fallback(spdlog::level::info);
        return logger;
    }
    return make_fallback(level);
}

void diagnostic(std::string const& message)
{
    std::fprintf(stderr, "lwm: %s\n", message.c_str());
    std::fflush(stderr);
}

bool owner_directory(fs::path const& directory)
{
    struct stat status {};
    if (::lstat(directory.c_str(), &status) != 0)
        return false;
    return S_ISDIR(status.st_mode) && status.st_uid == ::getuid() && (status.st_mode & 0077) == 0;
}

std::expected<fs::path, std::string> default_path()
{
    auto pid = static_cast<unsigned long long>(::getpid());
    if (char const* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
    {
        fs::path runtime_dir(runtime);
        // XDG_RUNTIME_DIR is an existing private directory, not a path to
        // create relative to the launcher's current working directory.
        if (runtime_dir.is_absolute() && owner_directory(runtime_dir))
        {
            fs::path log_dir = runtime_dir / "lwm";
            std::error_code ec;
            fs::create_directories(log_dir, ec);
            if (!ec)
            {
                ::chmod(log_dir.c_str(), 0700);
                if (owner_directory(log_dir))
                    return log_dir / ("lwm-" + std::to_string(pid) + ".log");
            }
        }
    }

    fs::path fallback = fs::path("/tmp") / ("lwm-" + std::to_string(pid) + ".log");
    struct stat tmp_status {};
    if (::stat("/tmp", &tmp_status) != 0 || !S_ISDIR(tmp_status.st_mode))
        return std::unexpected("cannot resolve a private runtime log directory");
    return fallback;
}

std::expected<fs::path, std::string> choose_path(LogOptions const& options)
{
    if (options.log_file)
        return *options.log_file;
    if (options.resolved_file_path)
        return *options.resolved_file_path;
    return default_path();
}

std::expected<void, std::string> validate_existing_path(fs::path const& path)
{
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0)
    {
        if (errno == ENOENT)
            return {};
        return std::unexpected("cannot inspect log file " + path.string() + ": " + std::strerror(errno));
    }
    if (S_ISLNK(status.st_mode))
        return std::unexpected("log file path must not be a symbolic link: " + path.string());
    if (!S_ISREG(status.st_mode))
        return std::unexpected("log file path is not a regular file: " + path.string());
    if (status.st_uid != ::getuid())
        return std::unexpected("log file is not owned by the current user: " + path.string());
    return {};
}

class secure_rotating_file_sink final : public spdlog::sinks::base_sink<std::mutex>
{
public:
    secure_rotating_file_sink(fs::path base_filename, size_t max_size, size_t max_files)
        : base_filename_(std::move(base_filename)), max_size_(max_size), max_files_(max_files)
    {
        if (max_size_ == 0)
            spdlog::throw_spdlog_ex("secure rotating sink: max_size cannot be zero");
        sanitize_existing_backups();
        open_file(false);
        enforce_active_size();
    }

    ~secure_rotating_file_sink() override
    {
        if (file_)
            std::fclose(file_);
    }

protected:
    void sink_it_(spdlog::details::log_msg const& message) override
    {
        if (disabled_)
            return;

        spdlog::memory_buf_t formatted;
        this->formatter_->format(message, formatted);
        size_t new_size = current_size_ + formatted.size();
        if (new_size > max_size_ && current_size_ > 0)
        {
            if (std::fflush(file_) != 0)
                spdlog::throw_spdlog_ex("secure rotating sink: flush failed for " + base_filename_.string(), errno);
            if (rotate_())
                new_size = formatted.size();
        }
        if (std::fwrite(formatted.data(), 1, formatted.size(), file_) != formatted.size())
            spdlog::throw_spdlog_ex("secure rotating sink: write failed for " + base_filename_.string(), errno);
        current_size_ = new_size;
    }

    void flush_() override
    {
        if (disabled_)
            return;
        if (std::fflush(file_) != 0)
            spdlog::throw_spdlog_ex("secure rotating sink: flush failed for " + base_filename_.string(), errno);
    }

private:
    fs::path filename(size_t index) const
    {
        if (index == 0)
            return base_filename_;
        fs::path stem = base_filename_;
        fs::path extension = stem.extension();
        stem.replace_extension();
        return fs::path(stem.string() + "." + std::to_string(index) + extension.string());
    }

    void close_file()
    {
        if (file_)
        {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    void open_file(bool truncate)
    {
        close_file();
        current_size_ = 0;
        fs::path path = filename(0);
        int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (truncate ? O_TRUNC : O_APPEND);
        int fd = ::open(path.c_str(), flags, 0600);
        if (fd < 0)
            spdlog::throw_spdlog_ex("secure rotating sink: cannot open " + path.string(), errno);

        struct stat stream_status {};
        struct stat path_status {};
        if (::fstat(fd, &stream_status) != 0)
        {
            int error = errno;
            ::close(fd);
            spdlog::throw_spdlog_ex("secure rotating sink: cannot inspect " + path.string(), error);
        }
        if (!S_ISREG(stream_status.st_mode) || stream_status.st_uid != ::getuid())
        {
            ::close(fd);
            spdlog::throw_spdlog_ex("secure rotating sink: unsafe log path " + path.string());
        }
        if (::lstat(path.c_str(), &path_status) != 0)
        {
            int error = errno;
            ::close(fd);
            spdlog::throw_spdlog_ex("secure rotating sink: cannot inspect " + path.string(), error);
        }
        if (S_ISLNK(path_status.st_mode) || path_status.st_dev != stream_status.st_dev
            || path_status.st_ino != stream_status.st_ino)
        {
            ::close(fd);
            spdlog::throw_spdlog_ex("secure rotating sink: unsafe log path " + path.string());
        }
        if (::fchmod(fd, 0600) != 0)
        {
            int error = errno;
            ::close(fd);
            spdlog::throw_spdlog_ex("secure rotating sink: cannot secure " + path.string(), error);
        }

        char const* mode = truncate ? "w" : "a";
        file_ = ::fdopen(fd, mode);
        if (!file_)
        {
            int error = errno;
            ::close(fd);
            spdlog::throw_spdlog_ex("secure rotating sink: cannot attach " + path.string(), error);
        }
        current_size_ = static_cast<size_t>(stream_status.st_size);
    }

    bool sanitize_rotation_source(fs::path const& path) const
    {
        struct stat path_status {};
        if (::lstat(path.c_str(), &path_status) != 0)
        {
            if (errno == ENOENT)
                return false;
            spdlog::throw_spdlog_ex("secure rotating sink: cannot inspect " + path.string(), errno);
        }
        if (!S_ISREG(path_status.st_mode) || path_status.st_uid != ::getuid())
            spdlog::throw_spdlog_ex("secure rotating sink: unsafe rotated log path " + path.string());

        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            spdlog::throw_spdlog_ex("secure rotating sink: cannot open " + path.string(), errno);

        struct stat stream_status {};
        if (::fstat(fd, &stream_status) != 0)
        {
            int error = errno;
            ::close(fd);
            spdlog::throw_spdlog_ex("secure rotating sink: cannot inspect " + path.string(), error);
        }
        if (!S_ISREG(stream_status.st_mode) || stream_status.st_uid != ::getuid()
            || stream_status.st_dev != path_status.st_dev || stream_status.st_ino != path_status.st_ino)
        {
            ::close(fd);
            spdlog::throw_spdlog_ex("secure rotating sink: unsafe rotated log path " + path.string());
        }
        if (::fchmod(fd, 0600) != 0)
        {
            int error = errno;
            ::close(fd);
            spdlog::throw_spdlog_ex("secure rotating sink: cannot secure " + path.string(), error);
        }
        ::close(fd);
        return true;
    }

    std::optional<size_t> backup_index(fs::path const& path) const
    {
        std::string name = path.filename().string();
        std::string stem = base_filename_.stem().string();
        std::string extension = base_filename_.extension().string();
        std::string prefix = stem + ".";
        if (!extension.empty())
        {
            if (!name.ends_with(extension))
                return std::nullopt;
            name.resize(name.size() - extension.size());
        }
        if (!name.starts_with(prefix))
            return std::nullopt;
        std::string digits = name.substr(prefix.size());
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char value) { return value >= '0' && value <= '9'; }))
            return std::nullopt;
        try
        {
            return static_cast<size_t>(std::stoull(digits));
        }
        catch (std::exception const&)
        {
            return std::nullopt;
        }
    }

    void discard_oversized_backup(fs::path const& path)
    {
        if (!sanitize_rotation_source(path))
            return;

        struct stat status {};
        if (::lstat(path.c_str(), &status) != 0)
        {
            if (errno == ENOENT)
                return;
            spdlog::throw_spdlog_ex("secure rotating sink: cannot inspect " + path.string(), errno);
        }
        if (static_cast<size_t>(status.st_size) <= max_size_)
            return;

        if (::unlink(path.c_str()) != 0 && errno != ENOENT)
            spdlog::throw_spdlog_ex("secure rotating sink: cannot remove oversized " + path.string(), errno);
        diagnostic("discarded oversized private log backup: " + path.string());
    }

    void sanitize_existing_backups()
    {
        for (size_t index = 1; index <= max_files_; ++index)
            discard_oversized_backup(filename(index));

        fs::path directory = base_filename_.parent_path();
        if (directory.empty())
            directory = ".";
        std::error_code error;
        fs::directory_iterator entries(directory, error);
        if (error)
            spdlog::throw_spdlog_ex("secure rotating sink: cannot inspect log directory " + directory.string(), error.value());
        for (auto const& entry : entries)
        {
            auto index = backup_index(entry.path());
            if (index && *index > max_files_)
            {
                if (!sanitize_rotation_source(entry.path()))
                    continue;
                if (::unlink(entry.path().c_str()) != 0 && errno != ENOENT)
                    spdlog::throw_spdlog_ex("secure rotating sink: cannot remove stale backup " + entry.path().string(), errno);
                diagnostic("discarded stale private log backup: " + entry.path().string());
            }
        }
    }

    void enforce_active_size()
    {
        if (current_size_ <= max_size_)
            return;
        diagnostic("truncating oversized private log at startup: " + base_filename_.string());
        open_file(true);
    }

    bool rotate_()
    {
        try
        {
            // Validate and secure every source and destination before closing
            // the active stream.  This leaves the sink recoverable on failure.
            for (size_t index = max_files_; index > 0; --index)
            {
                sanitize_rotation_source(filename(index - 1));
                sanitize_rotation_source(filename(index));
            }

            close_file();
            for (size_t index = max_files_; index > 0; --index)
            {
                fs::path source = filename(index - 1);
                struct stat source_status {};
                if (::lstat(source.c_str(), &source_status) != 0)
                {
                    if (errno == ENOENT)
                        continue;
                    spdlog::throw_spdlog_ex("secure rotating sink: cannot inspect " + source.string(), errno);
                }

                fs::path target = filename(index);
                if (::unlink(target.c_str()) != 0 && errno != ENOENT)
                    spdlog::throw_spdlog_ex("secure rotating sink: cannot replace " + target.string(), errno);
                if (::rename(source.c_str(), target.c_str()) != 0)
                    spdlog::throw_spdlog_ex("secure rotating sink: cannot rotate " + source.string(), errno);
            }
            open_file(false);
            rotation_degraded_ = false;
            return true;
        }
        catch (...)
        {
            std::string reason = "unknown rotation error";
            try
            {
                std::rethrow_exception(std::current_exception());
            }
            catch (std::exception const& error)
            {
                reason = error.what();
            }
            catch (...)
            {
            }

            try
            {
                open_file(false);
            }
            catch (std::exception const& recovery_error)
            {
                close_file();
                current_size_ = 0;
                disabled_ = true;
                {
                    std::lock_guard lock(state_mutex);
                    options_state.no_log_file = true;
                    options_state.log_file.reset();
                    options_state.resolved_file_path.reset();
                }
                diagnostic(std::string("private file logging disabled after rotation failure: ") + recovery_error.what());
                return false;
            }

            if (!rotation_degraded_)
                diagnostic("private log rotation failed; continuing without rotation: " + reason);
            rotation_degraded_ = true;
            return false;
        }
    }

    fs::path base_filename_;
    size_t max_size_;
    size_t max_files_;
    size_t current_size_ = 0;
    std::FILE* file_ = nullptr;
    bool disabled_ = false;
    bool rotation_degraded_ = false;
};

std::expected<spdlog::sink_ptr, std::string> make_file_sink(fs::path const& path)
{
    if (path.empty())
        return std::unexpected("log file path is empty");
    if (auto valid = validate_existing_path(path); !valid)
        return std::unexpected(valid.error());

    std::error_code ec;
    if (path.has_parent_path())
    {
        fs::create_directories(path.parent_path(), ec);
        if (ec)
            return std::unexpected("cannot create log directory " + path.parent_path().string() + ": " + ec.message());
    }

    try
    {
        auto sink = std::make_shared<secure_rotating_file_sink>(path, MAX_LOG_SIZE, MAX_LOG_FILES);
        sink->set_level(spdlog::level::warn);
        sink->set_formatter(make_formatter("[%Y-%m-%dT%H:%M:%S.%e] [%^%K%$] [%&] [%s:%#] %v"));
        return sink;
    }
    catch (std::exception const& error)
    {
        return std::unexpected("cannot open log file " + path.string() + ": " + error.what());
    }
}

std::shared_ptr<spdlog::logger> make_logger(LogOptions const& options, std::optional<fs::path> const& file_path)
{
    auto console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    switch (options.color)
    {
    case ColorMode::Always:
        console->set_color_mode(spdlog::color_mode::always);
        break;
    case ColorMode::Never:
        console->set_color_mode(spdlog::color_mode::never);
        break;
    case ColorMode::Auto:
        console->set_color_mode(spdlog::color_mode::automatic);
        break;
    }
    console->set_formatter(make_formatter("[%Y-%m-%dT%H:%M:%S.%e] [%^%K%$] [%&] %v"));
    console->set_level(options.level);

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(console);
    if (file_path)
    {
        auto file = make_file_sink(*file_path);
        if (!file)
            throw std::runtime_error(file.error());
        sinks.push_back(*file);
    }

    auto logger = std::make_shared<spdlog::logger>("lwm", sinks.begin(), sinks.end());
    auto gate = options.level;
    if (file_path && spdlog::level::warn < gate)
        gate = spdlog::level::warn;
    logger->set_level(gate);
    logger->flush_on(spdlog::level::warn);
    return logger;
}

std::shared_ptr<spdlog::logger> active_or_fallback_locked()
{
    if (!logger_state)
        logger_state = fallback_logger();
    return logger_state;
}

} // namespace

std::expected<spdlog::level::level_enum, std::string> parse_level(std::string_view value)
{
    if (value == "trace")
        return spdlog::level::trace;
    if (value == "debug")
        return spdlog::level::debug;
    if (value == "info")
        return spdlog::level::info;
    if (value == "warn" || value == "warning")
        return spdlog::level::warn;
    if (value == "error" || value == "err")
        return spdlog::level::err;
    if (value == "critical")
        return spdlog::level::critical;
    return std::unexpected("invalid log level '" + std::string(value) + "' (expected trace, debug, info, warn, error, or critical)");
}

std::expected<ColorMode, std::string> parse_color_mode(std::string_view value)
{
    if (value == "auto")
        return ColorMode::Auto;
    if (value == "always")
        return ColorMode::Always;
    if (value == "never")
        return ColorMode::Never;
    return std::unexpected("invalid log color mode '" + std::string(value) + "' (expected auto, always, or never)");
}

std::string level_name(spdlog::level::level_enum level)
{
    switch (level)
    {
    case spdlog::level::trace:
        return "TRACE";
    case spdlog::level::debug:
        return "DEBUG";
    case spdlog::level::info:
        return "INFO";
    case spdlog::level::warn:
        return "WARN";
    case spdlog::level::err:
        return "ERROR";
    case spdlog::level::critical:
        return "CRITICAL";
    case spdlog::level::off:
        return "OFF";
    default:
        return "UNKNOWN";
    }
}

std::shared_ptr<spdlog::logger> active_logger()
{
    std::lock_guard lock(state_mutex);
    return active_or_fallback_locked();
}

std::expected<void, std::string> initialize(LogOptions options)
{
    std::optional<fs::path> file_path;
    if (!options.no_log_file)
    {
        auto selected = choose_path(options);
        if (!selected)
        {
            diagnostic("default log file unavailable: " + selected.error() + "; using stderr only");
            options.no_log_file = true;
        }
        else
        {
            file_path = *selected;
        }
    }

    try
    {
        std::shared_ptr<spdlog::logger> candidate;
        try
        {
            candidate = make_logger(options, file_path);
        }
        catch (std::exception const& error)
        {
            if (options.log_file)
                return std::unexpected(error.what());
            diagnostic(std::string("default log file unavailable: ") + error.what() + "; using stderr only");
            options.no_log_file = true;
            candidate = make_logger(options, std::nullopt);
            file_path.reset();
        }

        options.resolved_file_path = file_path;
        std::lock_guard lock(state_mutex);
        logger_state = std::move(candidate);
        options_state = std::move(options);
        return {};
    }
    catch (std::exception const& error)
    {
        if (options.log_file)
            return std::unexpected(error.what());
        diagnostic(std::string("logging initialization failed: ") + error.what());
        return {};
    }
}

void init()
{
    auto result = initialize();
    if (!result)
        diagnostic(result.error());
}

LogOptions current_options()
{
    std::lock_guard lock(state_mutex);
    return options_state;
}

void flush()
{
    std::lock_guard lock(state_mutex);
    active_or_fallback_locked()->flush();
}

LogOptions prepare_exec()
{
    std::lock_guard lock(state_mutex);
    auto saved = options_state;
    active_or_fallback_locked()->flush();
    logger_state = fallback_logger(saved.level);
    options_state = {};
    return saved;
}

std::expected<void, std::string> restore(LogOptions const& options)
{
    return initialize(options);
}

void shutdown()
{
    std::lock_guard lock(state_mutex);
    active_or_fallback_locked()->flush();
    logger_state = fallback_logger();
    options_state = {};
}

} // namespace lwm::log
