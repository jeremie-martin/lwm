#include "lwm/core/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() > prefix.size() && value.starts_with(prefix);
}

std::optional<bool> inherited_descriptor(fs::path const& expected)
{
    try
    {
        fs::path normalized = fs::absolute(expected).lexically_normal();
        for (auto const& entry : fs::directory_iterator("/proc/self/fd"))
        {
            std::error_code error;
            fs::path target = fs::read_symlink(entry.path(), error);
            if (!error && fs::absolute(target).lexically_normal() == normalized)
                return true;
        }
        return false;
    }
    catch (std::filesystem::filesystem_error const&)
    {
        return std::nullopt;
    }
}

int check_child_descriptor(char const* executable, fs::path const& expected)
{
    pid_t child = ::fork();
    if (child < 0)
        return 125;
    if (child == 0)
    {
        ::execl(executable, executable, "--inspect-fd", expected.c_str(), nullptr);
        _exit(127);
    }

    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status))
        return 125;
    int result = WEXITSTATUS(status);
    return result == 125 ? 125 : result;
}

} // namespace

int main(int argc, char* argv[])
{
    lwm::log::LogOptions options;
    size_t burst = 1;
    bool prepare = false;
    bool restore = false;
    bool break_restore = false;
    bool block_default_file = false;
    bool symlink_backup = false;
    bool report_options = false;
    bool check_child_fd = false;
    bool post_rotation_record = false;
    bool emit_records = true;
    std::optional<fs::path> inspect_path;

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i] ? argv[i] : "";
        auto value = [&](std::string_view option) -> std::optional<std::string> {
            if (i + 1 >= argc)
            {
                std::cerr << "missing value for " << option << '\n';
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "--no-file")
        {
            options.no_log_file = true;
            continue;
        }
        if (arg == "--file" || starts_with(arg, "--file="))
        {
            auto path = arg == "--file" ? value("--file")
                                        : std::optional<std::string>(std::string(arg.substr(std::string_view("--file=").size())));
            if (!path)
                return 2;
            options.log_file = *path;
            continue;
        }
        if (arg == "--level" || starts_with(arg, "--level="))
        {
            auto level_text = arg == "--level" ? value("--level")
                                                : std::optional<std::string>(std::string(arg.substr(std::string_view("--level=").size())));
            if (!level_text)
                return 2;
            auto level = lwm::log::parse_level(*level_text);
            if (!level)
            {
                std::cerr << level.error() << '\n';
                return 2;
            }
            options.level = *level;
            continue;
        }
        if (arg == "--color" || starts_with(arg, "--color="))
        {
            auto color_text = arg == "--color" ? value("--color")
                                                : std::optional<std::string>(std::string(arg.substr(std::string_view("--color=").size())));
            if (!color_text)
                return 2;
            auto color = lwm::log::parse_color_mode(*color_text);
            if (!color)
            {
                std::cerr << color.error() << '\n';
                return 2;
            }
            options.color = *color;
            continue;
        }
        if (arg == "--burst")
        {
            auto count = value("--burst");
            if (!count)
                return 2;
            try
            {
                burst = std::stoul(*count);
            }
            catch (...)
            {
                std::cerr << "invalid burst count\n";
                return 2;
            }
            continue;
        }
        if (arg == "--prepare")
        {
            prepare = true;
            continue;
        }
        if (arg == "--restore")
        {
            restore = true;
            continue;
        }
        if (arg == "--break-restore")
        {
            break_restore = true;
            continue;
        }
        if (arg == "--block-default-file")
        {
            block_default_file = true;
            continue;
        }
        if (arg == "--symlink-backup")
        {
            symlink_backup = true;
            continue;
        }
        if (arg == "--report-options")
        {
            report_options = true;
            continue;
        }
        if (arg == "--check-child-fd")
        {
            check_child_fd = true;
            continue;
        }
        if (arg == "--post-rotation-record")
        {
            post_rotation_record = true;
            continue;
        }
        if (arg == "--no-records")
        {
            emit_records = false;
            continue;
        }
        if (arg == "--inspect-fd")
        {
            auto path = value("--inspect-fd");
            if (!path)
                return 2;
            inspect_path = *path;
            continue;
        }
        std::cerr << "unknown probe option: " << arg << '\n';
        return 2;
    }

    if (inspect_path)
    {
        auto inherited = inherited_descriptor(*inspect_path);
        if (!inherited)
            return 125;
        return *inherited ? 1 : 0;
    }

    if (block_default_file)
    {
        char const* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime)
        {
            std::cerr << "--block-default-file requires XDG_RUNTIME_DIR\n";
            return 2;
        }
        fs::path directory = fs::path(runtime) / "lwm";
        std::error_code error;
        fs::create_directories(directory, error);
        if (error)
        {
            std::cerr << "cannot create blocked log directory: " << error.message() << '\n';
            return 2;
        }
        fs::path target = directory / "blocked-target.log";
        fs::path blocked = directory / ("lwm-" + std::to_string(static_cast<unsigned long long>(::getpid())) + ".log");
        fs::remove(target, error);
        fs::remove(blocked, error);
        std::ofstream(target) << "blocked\n";
        if (::symlink(target.c_str(), blocked.c_str()) != 0)
        {
            std::cerr << "cannot create blocked log path\n";
            return 2;
        }
    }

    auto initialized = lwm::log::initialize(options);
    if (!initialized)
    {
        std::cerr << "logging initialization failed: " << initialized.error() << '\n';
        return 2;
    }

    auto active_options = lwm::log::current_options();
    auto selected = active_options.resolved_file_path;
    if (selected)
        std::cout << "resolved=" << selected->string() << '\n';
    if (report_options)
    {
        char const* color = active_options.color == lwm::log::ColorMode::Always
            ? "always"
            : active_options.color == lwm::log::ColorMode::Never ? "never" : "auto";
        std::cout << "level=" << lwm::log::level_name(active_options.level) << '\n'
                  << "color=" << color << '\n'
                  << "no_file=" << (active_options.no_log_file ? "1" : "0") << '\n'
                  << "resolved_present=" << (selected ? "1" : "0") << '\n';
    }

    if (symlink_backup)
    {
        if (!selected)
        {
            std::cerr << "--symlink-backup requires an active log file\n";
            return 3;
        }
        fs::path stem = *selected;
        fs::path extension = stem.extension();
        stem.replace_extension();
        fs::path backup = fs::path(stem.string() + ".2" + extension.string());
        fs::path target = backup.parent_path() / "rotation-target.log";
        std::error_code error;
        fs::remove(target, error);
        fs::remove(backup, error);
        std::ofstream(target) << "rotation target\n";
        if (::symlink(target.c_str(), backup.c_str()) != 0)
        {
            std::cerr << "cannot create rotation symlink\n";
            return 3;
        }
    }

    if (check_child_fd)
    {
        if (!selected)
        {
            std::cerr << "descriptor check requires an active log file\n";
            return 3;
        }
        int status = check_child_descriptor(argv[0], *selected);
        if (status == 125)
            return 125;
        if (status != 0)
        {
            std::cerr << "log descriptor inherited across exec (status " << status << ")\n";
            return 4;
        }
    }

    if (emit_records)
    {
        LOG_TRACE("probe trace");
        LOG_DEBUG("probe debug");
        LOG_INFO("probe info");
        LOG_WARN("probe warn");
        LOG_ERROR("probe error");
        LOG_CRITICAL("probe critical");
        LOG_KEY(0x12, 0x34);
        for (size_t i = 0; i < burst; ++i)
            LOG_WARN("probe burst {} {}", i, std::string(50000, 'x'));
        if (post_rotation_record)
            LOG_WARN("probe post-rotation record");
    }

    if (prepare)
    {
        auto saved = lwm::log::prepare_exec();
        LOG_INFO("probe fallback after prepare");
        if (break_restore)
        {
            auto path = saved.log_file ? saved.log_file : saved.resolved_file_path;
            if (path)
            {
                std::error_code error;
                fs::remove(*path, error);
                ::symlink("/tmp", path->c_str());
            }
        }
        if (restore)
        {
            auto restored = lwm::log::restore(saved);
            if (!restored)
            {
                std::cerr << "restore failed: " << restored.error() << '\n';
                LOG_WARN("probe fallback after failed restore");
                return 3;
            }
            LOG_WARN("probe restored after prepare");
        }
    }

    lwm::log::shutdown();
    LOG_INFO("probe after shutdown");
    return 0;
}
