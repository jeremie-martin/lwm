#include "cli.hpp"
#include "lwm/core/log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

#ifndef LWM_LOG_PROBE_PATH
#define LWM_LOG_PROBE_PATH "lwm_log_probe"
#endif
#ifndef LWM_BINARY_PATH
#define LWM_BINARY_PATH "lwm"
#endif

namespace {

struct ProcessResult
{
    int exit_code = -1;
    pid_t child_pid = -1;
    std::string stdout_text;
    std::string stderr_text;
};

fs::path make_temp_directory()
{
    std::string pattern = (fs::temp_directory_path() / "lwm-logging-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* result = ::mkdtemp(buffer.data());
    REQUIRE(result != nullptr);
    return result;
}

std::string read_file(fs::path const& path)
{
    std::ifstream input(path);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

ProcessResult run_process(char const* executable, std::vector<std::string> const& arguments,
                           std::optional<fs::path> runtime_directory = std::nullopt, bool clear_display = false)
{
    fs::path directory = make_temp_directory();
    fs::path stdout_path = directory / "stdout";
    fs::path stderr_path = directory / "stderr";
    int stdout_fd = ::open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int stderr_fd = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    REQUIRE(stdout_fd >= 0);
    REQUIRE(stderr_fd >= 0);

    pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::dup2(stdout_fd, STDOUT_FILENO);
        ::dup2(stderr_fd, STDERR_FILENO);
        ::close(stdout_fd);
        ::close(stderr_fd);
        if (runtime_directory && ::setenv("XDG_RUNTIME_DIR", runtime_directory->c_str(), 1) != 0)
            _exit(126);
        if (clear_display && ::unsetenv("DISPLAY") != 0)
            _exit(126);

        std::vector<std::string> child_strings;
        child_strings.emplace_back(executable);
        child_strings.insert(child_strings.end(), arguments.begin(), arguments.end());
        std::vector<char*> child_argv;
        child_argv.reserve(child_strings.size() + 1);
        for (std::string& argument : child_strings)
            child_argv.push_back(argument.data());
        child_argv.push_back(nullptr);
        ::execv(child_argv[0], child_argv.data());
        _exit(127);
    }

    ::close(stdout_fd);
    ::close(stderr_fd);
    int status = 0;
    bool finished = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!finished)
    {
        pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child)
        {
            finished = true;
            break;
        }
        if (waited < 0 && errno != EINTR)
        {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            finished = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            ::kill(child, SIGKILL);
            REQUIRE(::waitpid(child, &status, 0) == child);
            ProcessResult timed_out;
            timed_out.child_pid = child;
            timed_out.exit_code = -2;
            timed_out.stdout_text = read_file(stdout_path);
            timed_out.stderr_text = read_file(stderr_path);
            std::error_code error;
            fs::remove_all(directory, error);
            return timed_out;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ProcessResult result;
    result.child_pid = child;
    if (finished && WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    result.stdout_text = read_file(stdout_path);
    result.stderr_text = read_file(stderr_path);
    std::error_code error;
    fs::remove_all(directory, error);
    return result;
}

ProcessResult run_probe(std::vector<std::string> const& arguments, std::optional<fs::path> runtime_directory = std::nullopt)
{
    return run_process(LWM_LOG_PROBE_PATH, arguments, std::move(runtime_directory));
}

ProcessResult run_lwm(std::vector<std::string> const& arguments, bool clear_display = false)
{
    return run_process(LWM_BINARY_PATH, arguments, std::nullopt, clear_display);
}

std::vector<char*> mutable_argv(std::vector<std::string>& values)
{
    std::vector<char*> result;
    result.reserve(values.size());
    for (std::string& value : values)
        result.push_back(value.data());
    return result;
}

} // namespace

TEST_CASE("Logging probe routes levels and LOG_KEY to stderr without ANSI", "[logging]")
{
    auto result = run_probe({ "--no-file", "--level", "trace", "--color", "auto" });
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_text.empty());
    for (std::string const& marker : { "[TRACE]", "[DEBUG]", "[INFO]", "[WARN]", "[ERROR]", "[CRITICAL]", "Key: state=0x12 keysym=0x34" })
        REQUIRE(result.stderr_text.find(marker) != std::string::npos);
    REQUIRE(result.stderr_text.find("\033[") == std::string::npos);
    REQUIRE(result.stderr_text.find("[log_probe]") != std::string::npos);
}

TEST_CASE("Explicit color modes control ANSI output", "[logging]")
{
    auto always = run_probe({ "--no-file", "--level", "info", "--color", "always" });
    REQUIRE(always.exit_code == 0);
    REQUIRE(always.stderr_text.find("\033[") != std::string::npos);

    auto never = run_probe({ "--no-file", "--level", "info", "--color", "never" });
    REQUIRE(never.exit_code == 0);
    REQUIRE(never.stderr_text.find("\033[") == std::string::npos);
}

TEST_CASE("Default file path is PID-specific and survives lifecycle fallback", "[logging]")
{
    fs::path runtime_directory = make_temp_directory();
    auto result = run_probe({ "--level", "warn", "--prepare", "--restore" }, runtime_directory);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.child_pid > 0);

    fs::path expected = runtime_directory / "lwm" / ("lwm-" + std::to_string(result.child_pid) + ".log");
    REQUIRE(result.stdout_text.find("resolved=" + expected.string()) != std::string::npos);
    REQUIRE(fs::exists(expected));
    REQUIRE(read_file(expected).find("probe restored after prepare") != std::string::npos);

    struct stat status {};
    REQUIRE(::stat(expected.c_str(), &status) == 0);
    REQUIRE((status.st_mode & 0777) == 0600);
    fs::remove_all(runtime_directory);
}

TEST_CASE("Logging probe applies runtime gates and source lines to the private file", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path log_path = directory / "records.log";
    auto result = run_probe({ "--file", log_path.string(), "--level", "info" });
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_text.find("resolved=" + log_path.string()) != std::string::npos);
    REQUIRE(result.stderr_text.find("[INFO]") != std::string::npos);
    REQUIRE(result.stderr_text.find("[DEBUG]") == std::string::npos);
    REQUIRE(result.stderr_text.find("[TRACE]") == std::string::npos);

    std::string file = read_file(log_path);
    REQUIRE(file.find("[INFO]") == std::string::npos);
    REQUIRE(file.find("[WARN]") != std::string::npos);
    REQUIRE(file.find("[ERROR]") != std::string::npos);
    REQUIRE(file.find("[CRITICAL]") != std::string::npos);
    REQUIRE(file.find("[log_probe.cpp:") != std::string::npos);
    REQUIRE(file.find("\033[") == std::string::npos);

    struct stat status {};
    REQUIRE(::stat(log_path.c_str(), &status) == 0);
    REQUIRE((status.st_mode & 0777) == 0600);
    fs::remove_all(directory);
}

TEST_CASE("Private file retains warnings when console level is error", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path log_path = directory / "error-threshold.log";
    auto result = run_probe({ "--file", log_path.string(), "--level", "error", "--color", "never" });
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_text.find("[WARN]") == std::string::npos);
    REQUIRE(result.stderr_text.find("[ERROR]") != std::string::npos);

    std::string file = read_file(log_path);
    REQUIRE(file.find("[WARN]") != std::string::npos);
    REQUIRE(file.find("[ERROR]") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("Explicit log path failures are controlled", "[logging]")
{
    auto result = run_probe({ "--file", "/tmp", "--level", "trace" });
    REQUIRE(result.exit_code == 2);
    REQUIRE(result.stderr_text.find("logging initialization failed") != std::string::npos);
}

TEST_CASE("Default log path is private below XDG_RUNTIME_DIR", "[logging]")
{
    fs::path runtime = make_temp_directory();
    auto result = run_probe({ "--level", "warn" }, runtime);

    REQUIRE(result.exit_code == 0);
    std::string prefix = "resolved=" + (runtime / "lwm").string() + "/lwm-";
    REQUIRE(result.stdout_text.starts_with(prefix));
    std::string resolved = result.stdout_text.substr(std::string("resolved=").size());
    REQUIRE((!resolved.empty() && resolved.back() == '\n'));
    resolved.pop_back();
    fs::path path = resolved;
    struct stat status {};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    REQUIRE((status.st_mode & 0777) == 0600);
    REQUIRE(path.parent_path() == runtime / "lwm");

    struct stat directory_status {};
    REQUIRE(::lstat(path.parent_path().c_str(), &directory_status) == 0);
    REQUIRE(S_ISDIR(directory_status.st_mode));
    REQUIRE(directory_status.st_uid == ::getuid());
    REQUIRE((directory_status.st_mode & 0077) == 0);
    fs::remove_all(runtime);
}

TEST_CASE("Default log path falls back to a private /tmp file", "[logging]")
{
    auto result = run_probe({ "--level", "warn" }, fs::path("/proc/1/lwm-no-runtime"));

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_text.starts_with("resolved=/tmp/lwm-"));
    std::string resolved = result.stdout_text.substr(std::string("resolved=").size());
    REQUIRE((!resolved.empty() && resolved.back() == '\n'));
    resolved.pop_back();
    fs::path path = resolved;
    struct stat status {};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    REQUIRE((status.st_mode & 0777) == 0600);
    fs::remove(path);
}

TEST_CASE("Relative XDG runtime paths use the /tmp fallback", "[logging]")
{
    fs::path relative = "lwm-relative-runtime-" + std::to_string(static_cast<unsigned long long>(::getpid()));
    fs::remove_all(relative);
    auto result = run_probe({ "--level", "warn" }, relative);

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_text.starts_with("resolved=/tmp/lwm-"));
    REQUIRE(!fs::exists(relative));
    std::string resolved = result.stdout_text.substr(std::string("resolved=").size());
    REQUIRE((!resolved.empty() && resolved.back() == '\n'));
    resolved.pop_back();
    fs::remove(resolved);
}

TEST_CASE("Explicit symlink log paths are rejected", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path target = directory / "target.log";
    fs::path link = directory / "link.log";
    std::ofstream(target) << "untouched\n";
    REQUIRE(::symlink(target.c_str(), link.c_str()) == 0);

    auto result = run_probe({ "--file", link.string(), "--level", "trace" });
    REQUIRE(result.exit_code == 2);
    REQUIRE(result.stderr_text.find("logging initialization failed") != std::string::npos);
    REQUIRE(read_file(target) == "untouched\n");
    fs::remove_all(directory);
}

TEST_CASE("Implicit log open failures fall back to stderr", "[logging]")
{
    fs::path runtime = make_temp_directory();
    auto result = run_probe({ "--block-default-file", "--report-options", "--level", "warn" }, runtime);

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_text.find("resolved=") == std::string::npos);
    REQUIRE(result.stdout_text.find("no_file=1") != std::string::npos);
    REQUIRE(result.stderr_text.find("using stderr only") != std::string::npos);
    REQUIRE(result.stderr_text.find("[WARN]") != std::string::npos);
    fs::remove_all(runtime);
}

TEST_CASE("Prepare and restore move file logging around an exec boundary", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path log_path = directory / "lifecycle.log";
    auto result = run_probe({ "--file", log_path.string(), "--level", "info", "--color", "never", "--prepare", "--restore", "--report-options" });
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_text.find("probe fallback after prepare") != std::string::npos);
    REQUIRE(result.stderr_text.find("probe after shutdown") != std::string::npos);
    REQUIRE(result.stdout_text.find("level=INFO") != std::string::npos);
    REQUIRE(result.stdout_text.find("color=never") != std::string::npos);
    REQUIRE(result.stdout_text.find("no_file=0") != std::string::npos);
    REQUIRE(result.stdout_text.find("resolved_present=1") != std::string::npos);

    std::string file = read_file(log_path);
    REQUIRE(file.find("probe fallback after prepare") == std::string::npos);
    REQUIRE(file.find("probe restored after prepare") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("Failed restore keeps the configured fallback active", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path log_path = directory / "restore-failure.log";
    auto result = run_probe({ "--file", log_path.string(), "--level", "info", "--prepare", "--restore", "--break-restore" });
    REQUIRE(result.exit_code == 3);
    REQUIRE(result.stderr_text.find("restore failed") != std::string::npos);
    REQUIRE(result.stderr_text.find("probe fallback after failed restore") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("File logging descriptors are close-on-exec", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path log_path = directory / "inheritance.log";
    auto result = run_probe({ "--file", log_path.string(), "--level", "warn", "--check-child-fd" });
    if (result.exit_code == 125)
        SKIP("/proc/self/fd is unavailable");
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_text.find("log descriptor inherited across exec") == std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("Rotating file sink retains three backups with restrictive permissions", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path log_path = directory / "rotate.log";
    auto result = run_probe({ "--file", log_path.string(), "--level", "warn", "--burst", "100" });
    REQUIRE(result.exit_code == 0);
    REQUIRE(fs::exists(log_path));
    for (int index = 0; index <= 3; ++index)
    {
        fs::path path = index == 0
            ? log_path
            : log_path.parent_path() / (log_path.stem().string() + "." + std::to_string(index) + log_path.extension().string());
        REQUIRE(fs::exists(path));
        struct stat status {};
        REQUIRE(::stat(path.c_str(), &status) == 0);
        REQUIRE((status.st_mode & 0777) == 0600);
    }
    REQUIRE(!fs::exists(log_path.parent_path() / (log_path.stem().string() + ".4" + log_path.extension().string())));
    fs::remove_all(directory);
}

TEST_CASE("Preexisting permissive backups are secured before rotation", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path log_path = directory / "legacy-rotate.log";
    fs::path backup = directory / "legacy-rotate.1.log";
    std::ofstream(backup) << "legacy record\n";
    REQUIRE(::chmod(backup.c_str(), 0644) == 0);

    auto result = run_probe({ "--file", log_path.string(), "--level", "warn", "--burst", "25" });
    REQUIRE(result.exit_code == 0);
    fs::path rotated = directory / "legacy-rotate.2.log";
    REQUIRE(fs::exists(rotated));
    struct stat status {};
    REQUIRE(::stat(rotated.c_str(), &status) == 0);
    REQUIRE((status.st_mode & 0777) == 0600);
    REQUIRE(read_file(rotated).find("legacy record") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("Rotation failures keep later records in the active private file", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path log_path = directory / "recover-rotate.log";
    auto result = run_probe({ "--file", log_path.string(), "--level", "warn", "--burst", "25", "--symlink-backup", "--post-rotation-record" });
    REQUIRE(result.exit_code == 0);
    REQUIRE(fs::exists(log_path));
    REQUIRE(fs::file_size(log_path) > 0);
    REQUIRE(read_file(log_path).find("probe post-rotation record") != std::string::npos);
    REQUIRE(result.stderr_text.find("continuing without rotation") != std::string::npos);
    fs::path backup = directory / "recover-rotate.2.log";
    REQUIRE(fs::is_symlink(backup));
    fs::remove_all(directory);
}

TEST_CASE("Oversized active logs are bounded during initialization", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path log_path = directory / "oversized.log";
    {
        std::ofstream seed(log_path);
        seed << "seed";
    }
    REQUIRE(::truncate(log_path.c_str(), 2 * 1024 * 1024) == 0);
    fs::path oversized_backup = directory / "oversized.1.log";
    {
        std::ofstream seed(oversized_backup);
        seed << "seed";
    }
    REQUIRE(::truncate(oversized_backup.c_str(), 2 * 1024 * 1024) == 0);

    auto result = run_probe({ "--file", log_path.string(), "--level", "critical", "--no-records" });
    REQUIRE(result.exit_code == 0);
    REQUIRE(fs::file_size(log_path) <= 1024U * 1024U);
    REQUIRE(!fs::exists(oversized_backup));
    fs::remove_all(directory);
}

TEST_CASE("Stale numeric backups beyond retention are removed safely", "[logging]")
{
    fs::path directory = make_temp_directory();
    fs::path log_path = directory / "stale-rotate.log";
    fs::path stale = directory / "stale-rotate.4.log";
    std::ofstream(stale) << "stale record\n";
    REQUIRE(::chmod(stale.c_str(), 0644) == 0);

    auto result = run_probe({ "--file", log_path.string(), "--level", "critical", "--no-records" });
    REQUIRE(result.exit_code == 0);
    REQUIRE(!fs::exists(stale));
    fs::remove_all(directory);
}

TEST_CASE("CLI preserves one config path and restart argv", "[logging][cli]")
{
    std::vector<std::string> values { "lwm", "-V", "--config", "config.toml", "--log-color=never" };
    auto argv = mutable_argv(values);
    auto parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->log.level == spdlog::level::debug);
    REQUIRE(parsed->log.color == lwm::log::ColorMode::Never);
    REQUIRE(parsed->config_path == "config.toml");
    REQUIRE(parsed->restart_argv == values);

    values = { "lwm", "--config", "a.toml", "b.toml" };
    argv = mutable_argv(values);
    parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(!parsed.has_value());
    REQUIRE(parsed.error().find("duplicate config") != std::string::npos);

    values = { "lwm", "--config=" };
    argv = mutable_argv(values);
    parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(!parsed.has_value());
    REQUIRE(parsed.error().find("empty value for --config") != std::string::npos);

    values = { "lwm", "--log-file=" };
    argv = mutable_argv(values);
    parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(!parsed.has_value());
    REQUIRE(parsed.error().find("empty value for --log-file") != std::string::npos);

    values = { "lwm", "-c", "short.toml" };
    argv = mutable_argv(values);
    parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->config_path == "short.toml");
    REQUIRE(parsed->restart_argv == values);

    values = { "lwm", "-cattached.toml", "-dall" };
    argv = mutable_argv(values);
    parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->config_path == "attached.toml");
    REQUIRE(parsed->log.level == spdlog::level::trace);
    REQUIRE(parsed->restart_argv == values);

    values = { "lwm", "-dnope" };
    argv = mutable_argv(values);
    parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(!parsed.has_value());
    REQUIRE(parsed.error().find("invalid -d value") != std::string::npos);

    values = { "lwm", "-c" };
    argv = mutable_argv(values);
    parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(!parsed.has_value());
    REQUIRE(parsed.error().find("missing value") != std::string::npos);

    values = { "lwm", "-v" };
    argv = mutable_argv(values);
    parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->version);

    for (std::string const& literal : { "--debug", "--help", "--no-log-file", "--log-level", "--config" })
    {
        values = { "lwm", "--", literal };
        argv = mutable_argv(values);
        parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->config_path == literal);
        REQUIRE(parsed->log.level == spdlog::level::info);
        REQUIRE(!parsed->help);
        REQUIRE(!parsed->log.no_log_file);
    }

    values = { "lwm", "--", "first", "second" };
    argv = mutable_argv(values);
    parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(!parsed.has_value());
    REQUIRE(parsed.error().find("duplicate config") != std::string::npos);

    for (auto empty_path : std::vector<std::vector<std::string>> { { "lwm", "" }, { "lwm", "--", "" } })
    {
        argv = mutable_argv(empty_path);
        parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
        REQUIRE(!parsed.has_value());
        REQUIRE(parsed.error().find("empty config path") != std::string::npos);
    }
}

TEST_CASE("Real startup binary handles standalone version and help", "[logging][cli]")
{
    auto version = run_lwm({ "--version" });
    REQUIRE(version.exit_code == 0);
    REQUIRE(version.stdout_text == std::string(LWM_VERSION) + '\n');
    REQUIRE(version.stderr_text.empty());

    auto short_version = run_lwm({ "-v" });
    REQUIRE(short_version.exit_code == 0);
    REQUIRE(short_version.stdout_text == std::string(LWM_VERSION) + '\n');

    auto help = run_lwm({ "--help" });
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_text.empty());
    REQUIRE(help.stderr_text.find("-c, --config") != std::string::npos);
}

TEST_CASE("CLI rejects duplicate and conflicting logging options", "[logging][cli]")
{
    struct Case
    {
        std::vector<std::string> arguments;
        std::string expected_error;
    };
    for (auto const& test : std::vector<Case> {
             { { "lwm", "--log-level", "info", "--log-level=warn" }, "duplicate option: --log-level" },
             { { "lwm", "--log-color", "never", "--log-color=always" }, "duplicate option: --log-color" },
             { { "lwm", "--verbose", "--verbose" }, "duplicate option: --verbose" },
             { { "lwm", "--debug", "--debug" }, "duplicate option: --debug" },
             { { "lwm", "--log-file", "a.log", "--no-log-file" }, "conflicts with --log-file" },
             { { "lwm", "--no-log-file", "--log-file=a.log" }, "conflicts with --no-log-file" },
         })
    {
        auto values = test.arguments;
        auto argv = mutable_argv(values);
        auto parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
        REQUIRE(!parsed.has_value());
        REQUIRE(parsed.error().find(test.expected_error) != std::string::npos);
    }

    std::vector<std::string> separate_values { "lwm", "--log-level", "debug", "--log-file", "separate.log", "--log-color", "never" };
    auto argv = mutable_argv(separate_values);
    auto parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->log.level == spdlog::level::debug);
    REQUIRE(parsed->log.log_file == "separate.log");
    REQUIRE(parsed->log.color == lwm::log::ColorMode::Never);
}

TEST_CASE("Real startup binary reports logging initialization errors", "[logging][cli]")
{
    auto result = run_lwm({ "--log-file", "/tmp" });
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_text.find("logging initialization failed") != std::string::npos);

    auto invalid = run_lwm({ "--not-a-startup-option" });
    REQUIRE(invalid.exit_code == 2);
    REQUIRE(invalid.stderr_text.find("unknown option") != std::string::npos);
}

TEST_CASE("Real startup rejects an empty explicit config path", "[logging][cli]")
{
    auto result = run_lwm({ "" });
    REQUIRE(result.exit_code == 2);
    REQUIRE(result.stderr_text.find("empty config path") != std::string::npos);
    REQUIRE(result.stderr_text.find("using defaults") == std::string::npos);
}

TEST_CASE("Explicit malformed config is fatal at a high log threshold", "[logging][cli]")
{
    fs::path directory = make_temp_directory();
    fs::path config_path = directory / "invalid.toml";
    std::ofstream(config_path) << "[not valid\n";

    auto result = run_lwm({ "--config", config_path.string(), "--no-log-file", "--log-level", "error" }, true);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_text.find("[CRITICAL]") != std::string::npos);
    REQUIRE(result.stderr_text.find("Config parse error") != std::string::npos);
    REQUIRE(result.stderr_text.find("using defaults") == std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("Real startup reports fatal WM failures once at critical level", "[logging][cli]")
{
    fs::path directory = make_temp_directory();
    fs::path config_path = directory / "valid.toml";
    std::ofstream(config_path) << "# defaults\n";
    auto result = run_lwm(
        { "--config", config_path.string(), "--no-log-file", "--log-level", "critical", "--log-color", "never" },
        true
    );
    REQUIRE(result.exit_code == 1);
    auto first_critical = result.stderr_text.find("[CRITICAL]");
    REQUIRE(first_critical != std::string::npos);
    REQUIRE(result.stderr_text.find("[CRITICAL]", first_critical + 1) == std::string::npos);
    REQUIRE(result.stderr_text.find("giving up") != std::string::npos);
    fs::remove_all(directory);
}
