#include "cli.hpp"
#include "lwm/core/log.hpp"
#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <termios.h>

namespace fs = std::filesystem;
using namespace lwm::test;
using nlohmann::json;
namespace {
fs::path make_temp_directory() { return make_temp_dir(); }
std::vector<char*> mutable_argv(std::vector<std::string>& values)
{
    std::vector<char*> result;
    for (auto& value : values) result.push_back(value.data());
    return result;
}
CommandResult run_lwm(
    std::vector<std::string> args,
    bool clear_display = false,
    std::vector<std::pair<std::string, std::string>> env = { }
)
{
    if (clear_display)
        env.emplace_back("DISPLAY", "");
    if (env.empty())
        env.emplace_back("LWM_LOG_SOCKET", "/nonexistent/lwm-test-journal");
    auto result = run_command(LWM_BINARY_PATH, args, env);
    REQUIRE(result.has_value());
    return *result;
}
CommandResult probe(std::vector<std::string> args, std::string const& socket = "/nonexistent/lwm-test-journal")
{
    auto result = run_command(
        LWM_LOG_PROBE_PATH,
        args,
        {
            { "LWM_LOG_SOCKET", socket }
    }
    );
    REQUIRE(result.has_value());
    INFO(result->stdout_text);
    INFO(result->stderr_text);
    REQUIRE(result->exit_code == 0);
    return *result;
}
json status(CommandResult const& result)
{
    return json::parse(result.stdout_text.substr(result.stdout_text.find('{')));
}
struct Collector
{
    fs::path directory = make_temp_directory();
    LogCollector log{ (directory / "journal").string() };
    ~Collector() { fs::remove_all(directory); }
};
}

TEST_CASE("Journal preserves levels and source metadata and skips disabled arguments", "[logging]")
{
    Collector c;
    auto result = probe({ }, c.log.path);
    auto records = c.log.drain();
    REQUIRE(records.size() == 4);
    REQUIRE(records[0].at("MESSAGE") == "info");
    REQUIRE(records[0].at("PRIORITY") == "6");
    REQUIRE(records[1].at("PRIORITY") == "4");
    REQUIRE(records[2].at("PRIORITY") == "3");
    REQUIRE(records[3].at("PRIORITY") == "2");
    REQUIRE(records[0].at("CODE_FILE").ends_with("log_probe.cpp"));
    REQUIRE(std::stoi(records[0].at("CODE_LINE")) > 0);
    REQUIRE(records[0].at("CODE_FUNC") == "main");
    REQUIRE(records[0].at("SYSLOG_IDENTIFIER") == "lwm");
    REQUIRE(result.stdout_text.starts_with("evaluated=0"));
    REQUIRE(status(result)["delivery_drops"] == 0);
    REQUIRE(records[0].at("LWM_LOG_INSTANCE") == status(result)["instance"].get<std::string>());
}
TEST_CASE("Trace and off gates retain disabled argument semantics", "[logging]")
{
    auto trace = probe({ "--stderr", "--trace" });
    REQUIRE(trace.stdout_text.starts_with("evaluated=1"));
    REQUIRE(trace.stderr_text.find("trace") != std::string::npos);
    auto off = probe({ "--stderr", "--off" });
    REQUIRE(off.stdout_text.starts_with("evaluated=0"));
    REQUIRE(off.stderr_text.empty());
}
TEST_CASE("Stderr color is explicit and terminal control bytes are escaped", "[logging]")
{
    auto plain = probe({ "--stderr" });
    REQUIRE(plain.stderr_text.find("\033[") == std::string::npos);
    auto colored = probe({ "--stderr", "--color" });
    REQUIRE(colored.stderr_text.find("\033[") != std::string::npos);
    auto strings = probe({ "--stderr", "strings" });
    REQUIRE(strings.stderr_text.find("line one\\x0aPRIORITY=0") != std::string::npos);
}
TEST_CASE("Arguments are copied, bounded, and cannot inject journal fields", "[logging]")
{
    Collector c;
    auto result = probe({ "strings" }, c.log.path);
    auto records = c.log.drain();
    REQUIRE(records.size() == 4);
    REQUIRE(records[0].at("MESSAGE") == "owned-before-mutation");
    REQUIRE(records[1].at("MESSAGE").find("\nPRIORITY=0\n") != std::string::npos);
    REQUIRE(records[1].at("MESSAGE").find('\0') != std::string::npos);
    REQUIRE(records[1].at("PRIORITY") == "6");
    REQUIRE(records[2].at("MESSAGE").size() == 1024);
    REQUIRE(records[2].at("MESSAGE").ends_with("...[truncated]"));
    REQUIRE(records[3].at("MESSAGE").ends_with("...[truncated]"));
    REQUIRE(status(result)["truncations"] == 2);
}
TEST_CASE("An absent journal counts each failed delivery", "[logging]")
{
    auto result = probe({ });
    REQUIRE(status(result)["queue_drops"] == 0);
    REQUIRE(status(result)["delivery_drops"] == 4);
    REQUIRE(status(result)["last_delivery_error"] == ENOENT);
}
TEST_CASE("A full journal queue does not prevent shutdown and losses balance", "[logging]")
{
    Collector c;
    auto started = std::chrono::steady_clock::now();
    auto result = probe({ "burst" }, c.log.path);
    REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    auto state = status(result);
    auto records = c.log.drain();
    REQUIRE(state["queue_drops"].get<uint64_t>() > 0);
    REQUIRE(state["delivery_drops"].get<uint64_t>() > 0);
    REQUIRE(state["queue_drops"].get<uint64_t>() + state["delivery_drops"].get<uint64_t>() + records.size() == 100000);
}
TEST_CASE("Failed exec restores logging with the same instance and counters", "[logging]")
{
    Collector c;
    auto result = probe({ "restore" }, c.log.path);
    auto records = c.log.drain();
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].at("MESSAGE") == "before exec");
    REQUIRE(records[1].at("MESSAGE") == "after failed exec");
    REQUIRE(records[0].at("LWM_LOG_INSTANCE") == records[1].at("LWM_LOG_INSTANCE"));
    REQUIRE(status(result)["delivery_drops"] == 0);
}

TEST_CASE("CLI preserves one config path and restart argv", "[logging][cli]")
{
    std::vector<std::string> values{ "lwm", "-V", "--config", "config.toml", "--log-color=never" };
    auto argv = mutable_argv(values);
    auto parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->log.level == quill::LogLevel::Debug);
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
    REQUIRE(parsed.error().find("private log files were removed") != std::string::npos);

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
    REQUIRE(parsed->log.level == quill::LogLevel::TraceL3);
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

    for (std::string const& literal : { "--debug", "--help", "--log-target", "stderr", "--log-level", "--config" })
    {
        values = { "lwm", "--", literal };
        argv = mutable_argv(values);
        parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->config_path == literal);
        REQUIRE(parsed->log.level == quill::LogLevel::Info);
        REQUIRE(!parsed->help);
    }

    values = { "lwm", "--", "first", "second" };
    argv = mutable_argv(values);
    parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(!parsed.has_value());
    REQUIRE(parsed.error().find("duplicate config") != std::string::npos);

    for (auto empty_path : std::vector<std::vector<std::string>>{
             { "lwm", "" },
             { "lwm", "--", "" }
    })
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
    for (auto const& test : std::vector<Case>{
             {        { "lwm", "--log-level", "info", "--log-level=warn" },  "duplicate option: --log-level" },
             { { "lwm", "--log-target", "journal", "--log-target=stderr" }, "duplicate option: --log-target" },
             {                                   { "lwm", "--log-target" },                  "missing value" },
             {                              { "lwm", "--log-target=file" },             "invalid log target" },
             {                                  { "lwm", "--log-target=" },             "invalid log target" },
             {     { "lwm", "--log-color", "never", "--log-color=always" },  "duplicate option: --log-color" },
             {                         { "lwm", "--verbose", "--verbose" },    "duplicate option: --verbose" },
             {                             { "lwm", "--debug", "--debug" },      "duplicate option: --debug" },
             {           { "lwm", "--log-file", "a.log", "--no-log-file" }, "private log files were removed" },
             {              { "lwm", "--no-log-file", "--log-file=a.log" }, "private log files were removed" },
    })
    {
        auto values = test.arguments;
        auto argv = mutable_argv(values);
        auto parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
        REQUIRE(!parsed.has_value());
        REQUIRE(parsed.error().find(test.expected_error) != std::string::npos);
    }

    std::vector<std::string> separate_values{ "lwm",    "--log-level", "debug", "--log-target",
                                              "stderr", "--log-color", "never" };
    auto argv = mutable_argv(separate_values);
    auto parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->log.level == quill::LogLevel::Debug);
    REQUIRE(parsed->log.target == lwm::log::Target::Stderr);
    REQUIRE(parsed->log.color == lwm::log::ColorMode::Never);
}

TEST_CASE("Real startup binary reports logging initialization errors", "[logging][cli]")
{
    auto result = run_lwm(
        {
    },
        false,
        { { "LWM_LOG_SOCKET", "relative" } }
    );
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

    auto result = run_lwm({ "--config", config_path.string(), "--log-target", "stderr", "--log-level", "error" }, true);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_text.find("CRITICAL") != std::string::npos);
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
        { "--config",
          config_path.string(),
          "--log-target",
          "stderr",
          "--log-level",
          "critical",
          "--log-color",
          "never" },
        true
    );
    REQUIRE(result.exit_code == 1);
    auto first_critical = result.stderr_text.find("CRITICAL");
    REQUIRE(first_critical != std::string::npos);
    REQUIRE(result.stderr_text.find("CRITICAL", first_critical + 1) == std::string::npos);
    REQUIRE(result.stderr_text.find("giving up") != std::string::npos);
    fs::remove_all(directory);
}

namespace {
struct ChildProbe
{
    fs::path directory = make_temp_directory();
    pid_t pid = -1;
    int reader = -1;
    ChildProbe(std::string const& mode, std::string const& socket, std::string const& stderr_kind)
    {
        int output = open((directory / "stdout").c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        REQUIRE(output >= 0);
        int error = -1;
        if (stderr_kind == "full" || stderr_kind == "closed")
        {
            int pipefd[2];
            REQUIRE(pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0);
            reader = pipefd[0];
            error = pipefd[1];
            if (stderr_kind == "full")
            {
                std::string fill(4096, 'x');
                while (write(error, fill.data(), fill.size()) > 0)
                { }
                REQUIRE(errno == EAGAIN);
            }
            else
            {
                close(reader);
                reader = -1;
            }
            REQUIRE(fcntl(error, F_SETFL, fcntl(error, F_GETFL) & ~O_NONBLOCK) == 0);
        }
        else if (stderr_kind == "pty")
        {
            reader = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
            REQUIRE(reader >= 0);
            REQUIRE(grantpt(reader) == 0);
            REQUIRE(unlockpt(reader) == 0);
            error = open(ptsname(reader), O_WRONLY | O_NOCTTY | O_CLOEXEC);
            REQUIRE(error >= 0);
            termios settings{ };
            REQUIRE(tcgetattr(error, &settings) == 0);
            cfmakeraw(&settings);
            REQUIRE(tcsetattr(error, TCSANOW, &settings) == 0);
        }
        else if (stderr_kind == "file")
            error = open((directory / "stderr").c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        else
            error = open("/dev/null", O_WRONLY | O_CLOEXEC);
        REQUIRE(error >= 0);
        pid = fork();
        REQUIRE(pid >= 0);
        if (pid == 0)
        {
            dup2(output, STDOUT_FILENO);
            dup2(error, STDERR_FILENO);
            close(output);
            close(error);
            if (reader >= 0)
                close(reader);
            setenv("LWM_LOG_SOCKET", socket.c_str(), 1);
            if (stderr_kind == "journal")
                execl(LWM_LOG_PROBE_PATH, LWM_LOG_PROBE_PATH, mode.c_str(), nullptr);
            else
                execl(LWM_LOG_PROBE_PATH, LWM_LOG_PROBE_PATH, "--stderr", "--plain", mode.c_str(), nullptr);
            _exit(127);
        }
        close(output);
        close(error);
    }
    ~ChildProbe()
    {
        if (pid > 0)
        {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
        if (reader >= 0)
            close(reader);
        fs::remove_all(directory);
    }
    CommandResult finish()
    {
        int exit_status = 0;
        bool finished =
            wait_for_condition([&] { return waitpid(pid, &exit_status, WNOHANG) == pid; }, std::chrono::seconds(1));
        REQUIRE(finished);
        pid = -1;
        REQUIRE(WIFEXITED(exit_status));
        return { WEXITSTATUS(exit_status), read_text_file(directory / "stdout"), { } };
    }
};
}
TEST_CASE("Full and closed stderr pipes cannot block or kill the logger", "[logging]")
{
    for (std::string kind : { "full", "closed" })
    {
        ChildProbe child("levels", "", kind);
        auto result = child.finish();
        REQUIRE(result.exit_code == 0);
        REQUIRE(status(result)["delivery_drops"] == 4);
        REQUIRE(status(result)["last_delivery_error"] == (kind == "full" ? EAGAIN : EPIPE));
    }
}
TEST_CASE("Stderr rejects regular files with an actionable error", "[logging]")
{
    ChildProbe child("levels", "", "file");
    auto result = child.finish();
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stdout_text.find("pipe through tee") != std::string::npos);
}
TEST_CASE("Logger descriptors are close-on-exec", "[logging]")
{
    probe({ "cloexec" });
    probe({ "cloexec", "--stderr" });
}
TEST_CASE("Backend formatting failures are counted without recursive output", "[logging]")
{
    auto result = probe({ "backend-error", "--stderr" });
    REQUIRE(status(result)["backend_notifications"] == 1);
    REQUIRE_FALSE(status(result)["last_backend_notification"].get<std::string>().empty());
    REQUIRE(result.stderr_text.find("after formatting error") != std::string::npos);
}
TEST_CASE("Recurring warnings include Quill's suppression count", "[logging]")
{
    auto result = probe({ "rate-limit", "--stderr" });
    INFO(result.stderr_text);
    REQUIRE(result.stderr_text.find("(19x)") != std::string::npos);
}
TEST_CASE("Journal delivery resumes after receiver recreation", "[logging]")
{
    Collector c;
    ChildProbe child("paced", c.log.path, "journal");
    REQUIRE(wait_for_condition([&] { return !c.log.drain().empty(); }, std::chrono::milliseconds(200)));
    auto instance = c.log.text;
    unlink(c.log.path.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    LogCollector replacement(c.log.path);
    std::vector<std::map<std::string, std::string>> received;
    REQUIRE(wait_for_condition(
        [&]
        {
            received = replacement.drain();
            return !received.empty();
        },
        std::chrono::milliseconds(200)
    ));
    auto result = child.finish();
    REQUIRE(result.exit_code == 0);
    REQUIRE(status(result)["delivery_drops"].get<uint64_t>() > 0);
    REQUIRE(received[0].at("LWM_LOG_INSTANCE") == status(result)["instance"].get<std::string>());
}

TEST_CASE("Partial terminal writes resume without interleaving records", "[logging]")
{
    ChildProbe child("paced-large", "", "pty");
    // The PTY output capacity is smaller than this burst. Resume reading after
    // it has filled, so the sink must retain and finish a record before the next.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::string output;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline)
    {
        char buffer[257];
        ssize_t count = read(child.reader, buffer, sizeof(buffer));
        if (count > 0)
            output.append(buffer, count);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto result = child.finish();
    REQUIRE(result.exit_code == 0);
    REQUIRE(status(result)["delivery_drops"].get<uint64_t>() > 0);
    REQUIRE(output.ends_with("END\n"));
    size_t begin = 0;
    size_t lines = 0;
    while (begin < output.size())
    {
        auto end = output.find('\n', begin);
        REQUIRE(end != std::string::npos);
        auto line = output.substr(begin, end - begin);
        REQUIRE(line.starts_with("INFO [log_probe.cpp:"));
        REQUIRE(line.ends_with(" END"));
        REQUIRE(line.size() < 4096);
        REQUIRE(line.find("INFO", 1) == std::string::npos);
        ++lines;
        begin = end + 1;
    }
    REQUIRE(lines >= 2);
    REQUIRE(lines + status(result)["delivery_drops"].get<uint64_t>() == 30);
}

TEST_CASE("A failed logger restore stays inactive and exposes its error", "[logging]")
{
    Collector c;
    auto result = probe({ "failed-restore" }, c.log.path);
    auto records = c.log.drain();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].at("MESSAGE") == "before failed restore");
    REQUIRE(status(result)["active"] == false);
    REQUIRE(
        status(result)["initialization_error"].get<std::string>().find("absolute Unix socket path") != std::string::npos
    );
    REQUIRE(result.stderr_text.empty());
}

TEST_CASE("String views, null pointers, and unterminated arrays have bounded copied representations", "[logging]")
{
    Collector c;
    auto result = probe({ "boundaries" }, c.log.path);
    auto records = c.log.drain();
    REQUIRE(records.size() == 3);
    REQUIRE(records[0].at("MESSAGE") == "raw|(null)|");
    REQUIRE(records[1].at("MESSAGE").size() == 1024);
    REQUIRE(records[1].at("MESSAGE").ends_with("...[truncated]"));
    REQUIRE(records[2].at("MESSAGE") == "view before mutation");
    REQUIRE(status(result)["truncations"] == 1);
}
