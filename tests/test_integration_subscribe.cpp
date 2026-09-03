#include "x11_test_harness.hpp"
#include <X11/Xlib.h>
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xtest.h>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

struct TestEnvironment
{
    X11TestEnvironment& x11_env;
    X11Connection conn;
    LwmProcess wm;

    static std::optional<TestEnvironment> create()
    {
        auto& env = X11TestEnvironment::instance();
        if (!env.available())
        {
            WARN("Xvfb not available; set LWM_TEST_ALLOW_EXISTING_DISPLAY=1 to use an existing DISPLAY.");
            return std::nullopt;
        }

        X11Connection conn;
        if (!conn.ok())
        {
            WARN("Failed to connect to X server.");
            return std::nullopt;
        }

        LwmProcess wm(env.display());
        if (!wm.running())
        {
            WARN("Failed to start lwm.");
            return std::nullopt;
        }

        if (!wait_for_wm_ready(conn, kTimeout))
        {
            WARN("Window manager not ready.");
            return std::nullopt;
        }

        return TestEnvironment{ env, std::move(conn), std::move(wm) };
    }
};

std::optional<std::string> read_line_with_timeout(int fd, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string output;
    std::array<char, 512> buffer {};

    while (std::chrono::steady_clock::now() < deadline)
    {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd pfd { .fd = fd, .events = POLLIN | POLLHUP, .revents = 0 };
        int rc = poll(&pfd, 1, static_cast<int>(std::max<int64_t>(1, remaining.count())));
        if (rc <= 0)
            continue;
        if ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN))
            break;

        ssize_t n = read(fd, buffer.data(), buffer.size());
        if (n <= 0)
            break;

        output.append(buffer.data(), static_cast<size_t>(n));
        size_t newline = output.find('\n');
        if (newline != std::string::npos)
        {
            output.resize(newline + 1);
            return output;
        }
    }

    return std::nullopt;
}

std::string read_all_from_fd(int fd)
{
    std::string output;
    std::array<char, 512> buffer {};
    ssize_t n = 0;
    while ((n = read(fd, buffer.data(), buffer.size())) > 0)
        output.append(buffer.data(), static_cast<size_t>(n));
    return output;
}

bool wait_for_process_exit(pid_t pid, std::chrono::milliseconds timeout, int& status)
{
    return wait_for_condition(
        [&]()
        {
            pid_t result = waitpid(pid, &status, WNOHANG);
            return result == pid;
        },
        timeout
    );
}

std::optional<xcb_keycode_t> first_keycode_for_keysym(X11Connection& conn, xcb_keysym_t keysym)
{
    xcb_key_symbols_t* key_symbols = xcb_key_symbols_alloc(conn.get());
    if (!key_symbols)
        return std::nullopt;

    xcb_keycode_t* keycodes = xcb_key_symbols_get_keycode(key_symbols, keysym);
    std::optional<xcb_keycode_t> result;
    if (keycodes && keycodes[0] != XCB_NO_SYMBOL)
        result = keycodes[0];

    free(keycodes);
    xcb_key_symbols_free(key_symbols);
    return result;
}

bool send_key_chord(X11Connection& conn, xcb_keysym_t modifier, xcb_keysym_t key)
{
    auto modifier_code = first_keycode_for_keysym(conn, modifier);
    auto key_code = first_keycode_for_keysym(conn, key);
    if (!modifier_code || !key_code)
        return false;

    xcb_test_fake_input(conn.get(), XCB_KEY_PRESS, *modifier_code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_test_fake_input(conn.get(), XCB_KEY_PRESS, *key_code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_test_fake_input(conn.get(), XCB_KEY_RELEASE, *key_code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_test_fake_input(conn.get(), XCB_KEY_RELEASE, *modifier_code, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    xcb_flush(conn.get());
    return true;
}

void set_window_title(X11Connection& conn, xcb_window_t window, std::string const& title)
{
    xcb_atom_t net_wm_name = intern_atom(conn.get(), "_NET_WM_NAME");
    xcb_atom_t utf8_string = intern_atom(conn.get(), "UTF8_STRING");
    if (net_wm_name != XCB_NONE && utf8_string != XCB_NONE)
    {
        xcb_change_property(
            conn.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            net_wm_name,
            utf8_string,
            8,
            static_cast<uint32_t>(title.size()),
            title.data()
        );
    }

    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        window,
        XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING,
        8,
        static_cast<uint32_t>(title.size()),
        title.data()
    );
    xcb_flush(conn.get());
}

struct FocusChangeEvent
{
    std::string event;
    xcb_window_t window = XCB_NONE;
    std::string class_name;
    std::string title;
};

std::optional<std::string> parse_json_string(std::string_view line, size_t& position)
{
    if (position >= line.size() || line[position++] != '"')
        return std::nullopt;
    std::string result;
    while (position < line.size())
    {
        char ch = line[position++];
        if (ch == '"')
            return result;
        if (static_cast<unsigned char>(ch) < 0x20)
            return std::nullopt;
        if (ch != '\\')
        {
            result += ch;
            continue;
        }
        if (position >= line.size())
            return std::nullopt;
        char escaped = line[position++];
        switch (escaped)
        {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<FocusChangeEvent> parse_focus_change_event(std::string_view line)
{
    if (line.empty() || line.back() != '\n')
        return std::nullopt;
    line.remove_suffix(1);
    size_t position = 0;
    auto consume = [&](char expected)
    {
        return position < line.size() && line[position++] == expected;
    };
    auto key = [&](std::string_view expected)
    {
        auto value = parse_json_string(line, position);
        return value && *value == expected && consume(':');
    };
    auto number = [&]() -> std::optional<xcb_window_t>
    {
        if (position >= line.size() || line[position] < '0' || line[position] > '9')
            return std::nullopt;
        uint64_t value = 0;
        while (position < line.size() && line[position] >= '0' && line[position] <= '9')
        {
            value = value * 10 + static_cast<uint64_t>(line[position++] - '0');
            if (value > UINT32_MAX)
                return std::nullopt;
        }
        return static_cast<xcb_window_t>(value);
    };

    if (!consume('{') || !key("event"))
        return std::nullopt;
    auto event = parse_json_string(line, position);
    if (!event || !consume(',') || !key("window"))
        return std::nullopt;
    auto window = number();
    if (!window || !consume(',') || !key("class"))
        return std::nullopt;
    auto class_name = parse_json_string(line, position);
    if (!class_name || !consume(',') || !key("title"))
        return std::nullopt;
    auto title = parse_json_string(line, position);
    if (!title || !consume('}') || position != line.size())
        return std::nullopt;
    return FocusChangeEvent { std::move(*event), *window, std::move(*class_name), std::move(*title) };
}

} // namespace

TEST_CASE(
    "Integration: lwmctl subscribe exits when stdout consumer closes",
    "[integration][subscribe]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };
    REQUIRE(pipe(stdout_pipe) == 0);
    REQUIRE(pipe(stderr_pipe) == 0);

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0)
    {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        setenv("DISPLAY", wm.display().c_str(), 1);
        setenv("XDG_RUNTIME_DIR", wm.runtime_dir().c_str(), 1);

        std::filesystem::path executable = lwmctl_executable_path();
        execl(executable.c_str(), executable.c_str(), "subscribe", "window_map", nullptr);
        _exit(127);
    }

    close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    close(stderr_pipe[1]);
    stderr_pipe[1] = -1;

    auto cleanup_child = [&]()
    {
        if (stdout_pipe[0] >= 0)
            close(stdout_pipe[0]);
        if (stderr_pipe[0] >= 0)
            close(stderr_pipe[0]);

        if (pid > 0)
        {
            int status = 0;
            if (!wait_for_process_exit(pid, std::chrono::milliseconds(200), status))
            {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            pid = -1;
        }
    };

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);

    auto first_event = read_line_with_timeout(stdout_pipe[0], kTimeout);
    REQUIRE(first_event.has_value());
    REQUIRE(first_event->find("\"event\":\"window_map\"") != std::string::npos);

    close(stdout_pipe[0]);
    stdout_pipe[0] = -1;

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);

    int status = 0;
    bool exited = wait_for_process_exit(pid, kTimeout, status);
    if (!exited)
    {
        cleanup_child();
        FAIL("lwmctl subscribe did not exit after stdout closed");
    }

    std::string stderr_text = read_all_from_fd(stderr_pipe[0]);
    close(stderr_pipe[0]);
    stderr_pipe[0] = -1;
    pid = -1;
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(stderr_text.empty());

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: subscribe decodes escaped focus change fields",
    "[integration][subscribe][json]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    int stdout_pipe[2] = { -1, -1 };
    REQUIRE(pipe(stdout_pipe) == 0);

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0)
    {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        setenv("DISPLAY", wm.display().c_str(), 1);
        setenv("XDG_RUNTIME_DIR", wm.runtime_dir().c_str(), 1);

        std::filesystem::path executable = lwmctl_executable_path();
        execl(executable.c_str(), executable.c_str(), "subscribe", "focus_change", nullptr);
        _exit(127);
    }

    close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    xcb_window_t window = create_window(conn, 10, 10, 200, 150);
    set_window_wm_class(conn, window, "escaped-instance", "escaped-class");
    std::string title = "quote \" and slash \\ line\n tab\t";
    set_window_title(conn, window, title);
    map_window(conn, window);

    auto event_line = read_line_with_timeout(stdout_pipe[0], kTimeout);
    REQUIRE(event_line.has_value());
    auto event = parse_focus_change_event(*event_line);
    REQUIRE(event.has_value());
    CHECK(event->event == "focus_change");
    CHECK(event->window == window);
    CHECK(event->class_name == "escaped-class");
    CHECK(event->title == title);

    close(stdout_pipe[0]);
    stdout_pipe[0] = -1;
    int status = 0;
    if (!wait_for_process_exit(pid, std::chrono::milliseconds(200), status))
    {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    pid = -1;

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: lwmctl subscribe preserves monitor direction in key_action events",
    "[integration][subscribe]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    if (!extension_available(conn, &xcb_test_id))
        SKIP("XTEST extension not available");

    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };
    REQUIRE(pipe(stdout_pipe) == 0);
    REQUIRE(pipe(stderr_pipe) == 0);

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0)
    {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        setenv("DISPLAY", wm.display().c_str(), 1);
        setenv("XDG_RUNTIME_DIR", wm.runtime_dir().c_str(), 1);

        std::filesystem::path executable = lwmctl_executable_path();
        execl(executable.c_str(), executable.c_str(), "subscribe", "key_action", nullptr);
        _exit(127);
    }

    close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    close(stderr_pipe[1]);
    stderr_pipe[1] = -1;

    auto cleanup_child = [&]()
    {
        if (stdout_pipe[0] >= 0)
            close(stdout_pipe[0]);
        if (stderr_pipe[0] >= 0)
            close(stderr_pipe[0]);

        if (pid > 0)
        {
            int status = 0;
            if (!wait_for_process_exit(pid, std::chrono::milliseconds(200), status))
            {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            pid = -1;
        }
    };

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("Left")));
    auto left_event = read_line_with_timeout(stdout_pipe[0], kTimeout);
    REQUIRE(left_event.has_value());
    CHECK(left_event->find("\"event\":\"key_action\"") != std::string::npos);
    CHECK(left_event->find("\"action\":\"focus_monitor_left\"") != std::string::npos);

    REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("Right")));
    auto right_event = read_line_with_timeout(stdout_pipe[0], kTimeout);
    REQUIRE(right_event.has_value());
    CHECK(right_event->find("\"event\":\"key_action\"") != std::string::npos);
    CHECK(right_event->find("\"action\":\"focus_monitor_right\"") != std::string::npos);

    cleanup_child();
}
