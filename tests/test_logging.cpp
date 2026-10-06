#include "test_resources.hpp"
#include "cli.hpp"
#include "log_collector.hpp"
#include "lwm/core/log.hpp"
#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <nlohmann/json.hpp>

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
CommandResult run_lwm(std::vector<std::string> args, bool clear_display = false)
{
    if (std::find(args.begin(), args.end(), "--log-target") == args.end())
        args.insert(args.end(), { "--log-target", "stderr" });
    auto result = run_command(LWM_BINARY_PATH, args, clear_display
        ? std::vector<std::pair<std::string, std::string>>{{"DISPLAY", ""}} : std::vector<std::pair<std::string, std::string>>{});
    REQUIRE(result.has_value());
    return *result;
}
CommandResult probe(std::vector<std::string> args, std::string const& socket = "/nonexistent/lwm-test-journal")
{
    auto result = run_command(
        LWM_LOG_PROBE_PATH,
        args,
        {
            { "LWM_TEST_JOURNAL", socket }
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
void fill_journal(LogCollector const& collector)
{
    int sender = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    REQUIRE(sender >= 0);
    sockaddr_un address{ };
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, collector.path.c_str());
    constexpr std::string_view record = "MESSAGE=backpressure fixture\n";
    while (sendto(sender, record.data(), record.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) >= 0)
    { }
    int error = errno;
    close(sender);
    REQUIRE(error == EAGAIN);
}

}

TEST_CASE("Standard journal sink preserves levels and source metadata", "[logging]")
{
    Collector c;
    auto result = probe({ "--journal" }, c.log.path);
    auto records = c.log.drain();
    REQUIRE(records.size() == 4);
    REQUIRE(records[0].at("MESSAGE") == "info");
    REQUIRE(records[0].at("PRIORITY") == "6");
    REQUIRE(records[1].at("PRIORITY") == "4");
    REQUIRE(records[2].at("PRIORITY") == "3");
    REQUIRE(records[3].at("PRIORITY") == "2");
    REQUIRE(records[0].at("CODE_FILE") == "log_probe.cpp");
    REQUIRE(std::stoi(records[0].at("CODE_LINE")) > 0);
    REQUIRE(records[0].at("CODE_FUNC") == "main");
    REQUIRE(records[0].at("SYSLOG_IDENTIFIER") == "lwm");
    REQUIRE(result.stdout_text.starts_with("evaluated=0"));
    REQUIRE(status(result)["backend_notifications"] == 0);
}
TEST_CASE("Trace and off gates retain disabled argument semantics", "[logging]")
{
    auto trace = probe({ "--trace" });
    REQUIRE(trace.stdout_text.starts_with("evaluated=1"));
    REQUIRE(trace.stderr_text.find("trace") != std::string::npos);
    auto off = probe({ "--off" });
    REQUIRE(off.stdout_text.starts_with("evaluated=0"));
    REQUIRE(off.stderr_text.empty());
    REQUIRE(status(off)["active"] == false);
}
TEST_CASE("Standard sinks copy arguments and escape control bytes", "[logging]")
{
    Collector c;
    probe({ "--journal", "strings" }, c.log.path);
    auto records = c.log.drain();
    REQUIRE(records.size() == 4);
    REQUIRE(records[0].at("MESSAGE") == "owned-before-mutation");
    REQUIRE(records[1].at("MESSAGE") == "view-before-mutation");
    REQUIRE(records[2].at("PRIORITY") == "6");
    REQUIRE(records[2].at("MESSAGE").find("\nPRIORITY=0\n") != std::string::npos);
    REQUIRE(records[2].at("MESSAGE").find("\\x00") != std::string::npos);
    REQUIRE(records[3].at("MESSAGE") == std::string(8192, 'x'));
    auto console = probe({ "strings" });
    REQUIRE(console.stderr_text.find("] PRIORITY=0\n") != std::string::npos);
    REQUIRE(console.stderr_text.find("view-before-mutation") != std::string::npos);
}
TEST_CASE("Console color respects startup options", "[logging]")
{
    REQUIRE(probe({ "--plain" }).stderr_text.find("\033[") == std::string::npos);
    REQUIRE(probe({ "--color" }).stderr_text.find("\033[") != std::string::npos);
}
TEST_CASE("Absent journal is best effort and does not prevent shutdown", "[logging]")
{
    auto result = probe({ "--journal" });
    // libsystemd deliberately treats ENOENT as success; do not invent a delivery counter.
    REQUIRE(status(result)["backend_notifications"] == 0);
}
TEST_CASE("Shutdown drains records and disables subsequent argument evaluation", "[logging]")
{
    auto result = probe({ "stopped" });
    REQUIRE(result.stderr_text.find("before shutdown") != std::string::npos);
    REQUIRE(result.stderr_text.find("disabled while stopped") == std::string::npos);
}
TEST_CASE("Backend formatting errors and oversized records do not prevent subsequent logging", "[logging]")
{
    for (auto mode : { "backend-error", "oversized" })
    {
        auto result = probe({ mode });
        REQUIRE(status(result)["backend_notifications"].get<int>() > 0);
        REQUIRE_FALSE(status(result)["last_backend_notification"].get<std::string>().empty());
        REQUIRE(result.stderr_text.find("after ") != std::string::npos);
    }
}
TEST_CASE("Recurring warnings include Quill's suppression count", "[logging]")
{
    REQUIRE(probe({ "rate-limit" }).stderr_text.find("(19x)") != std::string::npos);
}

TEST_CASE("CLI preserves explicit configuration and restart arguments", "[logging][cli]")
{
    for (auto values : std::vector<std::vector<std::string>>{
             { "lwm", "--log-level", "debug", "--config", "config with spaces.toml", "--log-color=never" },
             { "lwm", "--log-level=debug", "--config=config with spaces.toml", "--log-color", "never" }
    })
    {
        auto argv = mutable_argv(values);
        auto parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->log.level == quill::LogLevel::Debug);
        CHECK(parsed->log.color == lwm::log::ColorMode::Never);
        CHECK(parsed->config_path == "config with spaces.toml");
        CHECK(parsed->restart_argv == values);
    }
    // An explicit option value is literal, including option-like paths and '='.
    for (std::string const& path : { "--help", "--log-level", "a=b.toml", "-config.toml" })
        for (bool attached : { false, true })
        {
            std::vector<std::string> values = attached
                ? std::vector<std::string>{ "lwm", "--config=" + path }
                : std::vector<std::string>{ "lwm", "--config", path };
            auto argv = mutable_argv(values);
            auto parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
            REQUIRE(parsed);
            CHECK(parsed->config_path == path);
            CHECK_FALSE(parsed->help);
            CHECK(parsed->restart_argv == values);
        }
}

TEST_CASE("CLI has one validation path for every value option", "[logging][cli]")
{
    for (std::string const& option : { "--config", "--log-level", "--log-target", "--log-color" })
    {
        CAPTURE(option);
        for (auto values : std::vector<std::vector<std::string>>{
                 { "lwm", option }, { "lwm", option, "" }, { "lwm", option + "=" }
        })
        {
            auto argv = mutable_argv(values);
            auto parsed = lwm::cli::parse(static_cast<int>(argv.size()), argv.data());
            REQUIRE_FALSE(parsed);
            CHECK(parsed.error() == (values.back() == option ? "missing value for " : "empty value for ") + option);
        }
    }
}

TEST_CASE("Unsupported startup arguments use ordinary rejection", "[logging][cli]")
{
    for (std::string const& argument : {
             "config.toml", "", "--", "-c", "-cconfig.toml", "-V", "--verbose", "--debug",
             "-d", "-dall", "--log-file", "--log-file=a.log", "--no-log-file", "--unknown"
    })
    {
        CAPTURE(argument);
        auto result = run_lwm({ argument }, true);
        CHECK(result.exit_code == 2);
        CHECK(result.stderr_text.find("unknown argument: " + argument + "\n") != std::string::npos);
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
    REQUIRE(help.stderr_text.find("--config PATH") != std::string::npos);
}

TEST_CASE("CLI rejects duplicate options and invalid values", "[logging][cli]")
{
    struct Case
    {
        std::vector<std::string> arguments;
        std::string expected_error;
    };
    for (auto const& test : std::vector<Case>{
             { { "lwm", "--config", "a.toml", "--config=b.toml" }, "duplicate option: --config" },
             { { "lwm", "--log-level=warning" }, "invalid log level" },
             { { "lwm", "--log-level=err" }, "invalid log level" },
             {        { "lwm", "--log-level", "info", "--log-level=warn" },  "duplicate option: --log-level" },
             { { "lwm", "--log-target", "journal", "--log-target=stderr" }, "duplicate option: --log-target" },
             {                                   { "lwm", "--log-target" },                  "missing value" },
             {                              { "lwm", "--log-target=file" },             "invalid log target" },
             {                                  { "lwm", "--log-target=" },             "empty value for --log-target" },
             {     { "lwm", "--log-color", "never", "--log-color=always" },  "duplicate option: --log-color" },
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

TEST_CASE("Real startup rejects an empty explicit config path", "[logging][cli]")
{
    auto result = run_lwm({ "--config", "" });
    REQUIRE(result.exit_code == 2);
    REQUIRE(result.stderr_text.find("empty value for --config") != std::string::npos);
    REQUIRE(result.stderr_text.find("using defaults") == std::string::npos);
}

TEST_CASE("Explicit malformed config falls back to defaults at a high log threshold", "[logging][cli]")
{
    fs::path directory = make_temp_directory();
    fs::path config_path = directory / "invalid.toml";
    std::ofstream(config_path) << "[not valid\n";

    // Without a display, startup still fails afterwards, on the X connection.
    auto result = run_lwm({ "--config", config_path.string(), "--log-target", "stderr", "--log-level", "error" }, true);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_text.find("CRITICAL") != std::string::npos);
    REQUIRE(result.stderr_text.find("Config error") != std::string::npos);
    REQUIRE(result.stderr_text.find(config_path.string()) != std::string::npos);
    REQUIRE(result.stderr_text.find("using the default configuration") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("Configuration checks report errors without a display", "[logging][cli]")
{
    fs::path directory = make_temp_directory();
    fs::path valid = directory / "valid.toml";
    fs::path invalid = directory / "invalid.toml";
    std::ofstream(valid) << "[appearance]\nborder_width = 3\n";
    std::ofstream(invalid) << "[appearance]\nborder_width = -1\n";
    auto passed = run_lwm({ "--check-config", "--config", valid.string() }, true);
    CHECK(passed.exit_code == 0);
    CHECK(passed.stdout_text.empty());
    CHECK(passed.stderr_text.empty());
    auto failed = run_lwm({ "--check-config", "--config", invalid.string() }, true);
    CHECK(failed.exit_code == 1);
    CHECK(failed.stderr_text.find("border_width") != std::string::npos);
    auto missing = run_lwm({ "--check-config", "--config", (directory / "absent.toml").string() }, true);
    CHECK(missing.exit_code == 1);
    CHECK(missing.stderr_text.find("does not exist") != std::string::npos);
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
    REQUIRE(result.stderr_text.find("WM initialization failed") != std::string::npos);
    fs::remove_all(directory);
}

namespace {
struct ChildProbe
{
    fs::path directory = make_temp_directory();
    pid_t pid = -1;
    lwm::test::TestFd reader_owner;
    int& reader = reader_owner.fd;
    ChildProbe(std::string const& mode, std::string const& destination, std::string const& socket = "")
    {
        lwm::test::TestFd output_owner{ open((directory / "stdout").c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600) };
        int output = output_owner.fd;
        REQUIRE(output >= 0);
        lwm::test::TestFd error_owner;
        int& error = error_owner.fd;
        if (destination == "full" || destination == "closed")
        {
            int pipefd[2];
            REQUIRE(pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0);
            reader = pipefd[0];
            error = pipefd[1];
            if (destination == "full")
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
        else
            error = open((directory / "stderr").c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
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
            setenv("LWM_TEST_JOURNAL", socket.c_str(), 1);
            execl(
                LWM_LOG_PROBE_PATH,
                LWM_LOG_PROBE_PATH,
                destination == "journal" ? "--journal" : "--plain",
                mode.c_str(),
                nullptr
            );
            _exit(127);
        }
    }
    ~ChildProbe()
    {
        if (pid > 0)
        {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
        fs::remove_all(directory);
    }
    std::string output() { return read_text_file(directory / "stdout"); }
    CommandResult finish(std::function<void()> drain = [] { })
    {
        int exit_status = 0;
        REQUIRE(wait_for_condition(
            [&]
            {
                drain();
                return waitpid(pid, &exit_status, WNOHANG) == pid;
            },
            std::chrono::seconds(3)
        ));
        pid = -1;
        REQUIRE(WIFEXITED(exit_status));
        return { WEXITSTATUS(exit_status), output(), read_text_file(directory / "stderr") };
    }
};
}
TEST_CASE("Standard sinks isolate submission from backpressure and drain when the reader resumes", "[logging]")
{
    for (std::string target : { "full", "journal" })
    {
        CAPTURE(target);
        Collector c;
        if (target == "journal")
            fill_journal(c.log);
        ChildProbe child("burst", target, c.log.path);
        // All 100,000 calls must finish while the destination remains unread.
        REQUIRE(wait_for_condition(
            [&] { return child.output().find("submitted") != std::string::npos; },
            std::chrono::seconds(1)
        ));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        siginfo_t info{ };
        REQUIRE(waitid(P_PID, child.pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0);
        REQUIRE(info.si_pid == 0); // Standard shutdown waits for delivery.
        auto result = child.finish(
            [&]
            {
                if (target == "journal")
                    c.log.drain();
                else
                {
                    char data[8192];
                    while (read(child.reader, data, sizeof(data)) > 0)
                    { }
                }
            }
        );
        REQUIRE(result.exit_code == 0);
        REQUIRE(status(result)["backend_notifications"].get<int>() > 0);
        REQUIRE(status(result)["last_backend_notification"].get<std::string>().find("ropped") != std::string::npos);
    }
}
TEST_CASE("A closed console pipe is reported without terminating the WM", "[logging]")
{
    ChildProbe child("levels", "closed");
    auto result = child.finish();
    REQUIRE(result.exit_code == 0);
    REQUIRE(status(result)["backend_notifications"].get<int>() > 0);
}
TEST_CASE("Ordinary stderr file redirection works", "[logging]")
{
    ChildProbe child("levels", "file");
    auto result = child.finish();
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_text.find("CRITICAL") != std::string::npos);
    REQUIRE(status(result)["backend_notifications"] == 0);
}

TEST_CASE("The standard journal transport does not leak its socket through exec", "[logging]")
{
    Collector c;
    probe({ "--journal", "cloexec" }, c.log.path);
    REQUIRE(c.log.drain().size() == 4);
}

TEST_CASE("Exec replaces a blocked worker without waiting for the destination", "[logging]")
{
    for (std::string target : { "full", "journal" })
    {
        Collector c;
        if (target == "journal")
            fill_journal(c.log);
        ChildProbe child("exec-blocked", target, c.log.path);
        auto result = child.finish();
        REQUIRE(result.exit_code == 0);
        REQUIRE(result.stdout_text.find("exec replaced worker") != std::string::npos);
    }
}

#ifndef NDEBUG
TEST_CASE("An invariant failure aborts even when diagnostic output is blocked", "[logging]")
{
    ChildProbe child("invariant", "full");
    int status = 0;
    REQUIRE(
        wait_for_condition([&] { return waitpid(child.pid, &status, WNOHANG) == child.pid; }, std::chrono::seconds(3))
    );
    child.pid = -1;
    REQUIRE(WIFSIGNALED(status));
    REQUIRE(WTERMSIG(status) == SIGABRT);
}
#endif
