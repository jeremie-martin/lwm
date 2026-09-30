#pragma once

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

namespace lwm::test {

inline std::optional<std::string> find_in_path(char const* name)
{
    char const* path = std::getenv("PATH");
    if (!path)
        return std::nullopt;

    std::string paths(path);
    size_t start = 0;
    while (start < paths.size())
    {
        size_t end = paths.find(':', start);
        if (end == std::string::npos)
            end = paths.size();
        std::string candidate = paths.substr(start, end - start) + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0)
            return candidate;
        start = end + 1;
    }
    return std::nullopt;
}

inline bool wait_for_condition(std::function<bool()> const& predicate, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

class X11TestEnvironment
{
public:
    static X11TestEnvironment& instance()
    {
        static X11TestEnvironment env;
        return env;
    }

    bool available() const
    {
        if (std::getenv("LWM_TEST_REQUIRE_X11"))
            REQUIRE(available_);
        return available_;
    }
    bool owns_display() const { return owns_display_; }
    std::string const& display() const { return display_; }

private:
    X11TestEnvironment()
    {
        char const* allow_existing = std::getenv("LWM_TEST_ALLOW_EXISTING_DISPLAY");
        bool use_existing = allow_existing && std::strcmp(allow_existing, "1") == 0;

        if (start_server())
        {
            available_ = true;
            return;
        }

        if (use_existing)
        {
            char const* existing = std::getenv("DISPLAY");
            if (existing && *existing)
            {
                display_ = existing;
                available_ = true;
            }
        }
    }

    ~X11TestEnvironment()
    {
        stop_server();
        restore_display();
    }

    X11TestEnvironment(X11TestEnvironment const&) = delete;
    X11TestEnvironment& operator=(X11TestEnvironment const&) = delete;

    bool start_server()
    {
        char const* requested_server = std::getenv("LWM_TEST_XSERVER");
        bool nested = requested_server && std::strcmp(requested_server, "Xephyr") == 0;
        bool dummy = requested_server && std::strcmp(requested_server, "Xorg") == 0;
        auto server = find_in_path(dummy ? "Xorg" : nested ? "Xephyr" : "Xvfb");
        // Debian's console-only wrapper is unnecessary for a rootless dummy server.
        if (dummy && access("/usr/lib/xorg/Xorg", X_OK) == 0)
            server = "/usr/lib/xorg/Xorg";
        if (!server)
            return false;

        char const* previous = std::getenv("DISPLAY");
        if (previous && *previous)
            previous_display_ = std::string(previous);

        // Let the server reserve a free display atomically and report it when ready.
        int ready[2];
        if (pipe(ready) != 0)
            return false;
        std::string ready_fd = std::to_string(ready[1]);
        pid_t pid = fork();
        if (pid == 0)
        {
            close(ready[0]);
            if (dummy)
            {
                execl(
                    server->c_str(),
                    "Xorg",
                    "-displayfd",
                    ready_fd.c_str(),
                    "-config",
                    LWM_XORG_CONFIG_PATH,
                    "-noreset",
                    "-nolisten",
                    "tcp",
                    nullptr
                );
                _exit(127);
            }
            if (nested)
            {
                execl(
                    server->c_str(),
                    "Xephyr",
                    "-displayfd",
                    ready_fd.c_str(),
                    "-screen",
                    "1280x720",
                    "-nolisten",
                    "tcp",
                    nullptr
                );
                _exit(127);
            }
            execl(
                server->c_str(),
                "Xvfb",
                "-displayfd",
                ready_fd.c_str(),
                "-screen",
                "0",
                "1280x720x24",
                "-nolisten",
                "tcp",
                nullptr
            );
            _exit(127);
        }
        close(ready[1]);
        if (pid < 0)
        {
            close(ready[0]);
            return false;
        }

        server_pid_ = pid;
        owns_display_ = true;
        std::string number;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && number.size() < 16)
        {
            pollfd fd{ ready[0], POLLIN, 0 };
            int result = poll(&fd, 1, 100);
            if (result < 0 && errno == EINTR)
                continue;
            if (result < 0)
                break;
            if (result == 0)
                continue;
            char ch;
            if (read(ready[0], &ch, 1) != 1)
                break;
            if (ch == '\n' && !number.empty())
            {
                close(ready[0]);
                display_ = ":" + number;
                setenv("DISPLAY", display_.c_str(), 1);
                display_modified_ = true;
                return true;
            }
            if (ch < '0' || ch > '9')
                break;
            number += ch;
        }
        close(ready[0]);
        stop_server();
        return false;
    }

    void stop_server()
    {
        if (!owns_display_ || server_pid_ <= 0)
            return;

        kill(server_pid_, SIGTERM);
        bool exited = wait_for_condition(
            [this]() { return waitpid(server_pid_, nullptr, WNOHANG) > 0; },
            std::chrono::milliseconds(1000)
        );
        if (!exited)
        {
            kill(server_pid_, SIGKILL);
            waitpid(server_pid_, nullptr, 0);
        }
        server_pid_ = -1;
        owns_display_ = false;
    }

    void restore_display()
    {
        if (!display_modified_)
            return;
        if (previous_display_)
            setenv("DISPLAY", previous_display_->c_str(), 1);
        else
            unsetenv("DISPLAY");
        display_modified_ = false;
    }

    bool available_ = false;
    bool owns_display_ = false;
    pid_t server_pid_ = -1;
    std::string display_;
    std::optional<std::string> previous_display_;
    bool display_modified_ = false;
};

class X11Connection
{
public:
    X11Connection()
        : conn_(nullptr)
        , screen_(nullptr)
    {
        // Xvfb briefly refuses connections while resetting between test cases.
        wait_for_condition(
            [&]()
            {
                if (conn_)
                    xcb_disconnect(conn_);
                // XCB's handshake uses writev: a resetting server can raise SIGPIPE
                // before xcb_connect returns its connection error. Suppress only
                // during this synchronous attempt; WM children keep normal signals.
                auto previous = ::signal(SIGPIPE, SIG_IGN);
                conn_ = xcb_connect(nullptr, nullptr);
                ::signal(SIGPIPE, previous);
                return conn_ && !xcb_connection_has_error(conn_);
            },
            std::chrono::seconds(1)
        );
        if (conn_ && !xcb_connection_has_error(conn_))
            screen_ = xcb_setup_roots_iterator(xcb_get_setup(conn_)).data;
    }

    ~X11Connection()
    {
        if (conn_)
            xcb_disconnect(conn_);
    }

    X11Connection(X11Connection const&) = delete;
    X11Connection& operator=(X11Connection const&) = delete;

    X11Connection(X11Connection&& other) noexcept
        : conn_(other.conn_)
        , screen_(other.screen_)
    {
        other.conn_ = nullptr;
        other.screen_ = nullptr;
    }

    X11Connection& operator=(X11Connection&& other) noexcept
    {
        if (this == &other)
            return *this;

        if (conn_)
            xcb_disconnect(conn_);

        conn_ = other.conn_;
        screen_ = other.screen_;
        other.conn_ = nullptr;
        other.screen_ = nullptr;
        return *this;
    }

    bool ok() const { return conn_ && !xcb_connection_has_error(conn_) && screen_; }
    xcb_connection_t* get() const { return conn_; }
    xcb_screen_t* screen() const { return screen_; }
    xcb_window_t root() const { return screen_->root; }

private:
    xcb_connection_t* conn_;
    xcb_screen_t* screen_;
};

inline bool extension_available(X11Connection& conn, xcb_extension_t* extension_id)
{
    auto* extension = xcb_get_extension_data(conn.get(), extension_id);
    return extension && extension->present;
}

inline xcb_atom_t intern_atom(xcb_connection_t* conn, char const* name)
{
    auto cookie = xcb_intern_atom(conn, 0, static_cast<uint16_t>(std::strlen(name)), name);
    auto* reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    if (!reply)
        return XCB_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

// Missing properties and failed X requests are different outcomes. Negative
// assertions must never turn a failed request or a malformed value into absence.
inline std::optional<std::vector<uint32_t>>
read_property32(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom, xcb_atom_t type)
{
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 65536);
    std::unique_ptr<xcb_get_property_reply_t, decltype(&free)> reply(
        xcb_get_property_reply(conn, cookie, nullptr),
        &free
    );
    REQUIRE(reply);
    if (reply->type == XCB_NONE)
        return std::nullopt;
    REQUIRE(reply->type == type);
    REQUIRE(reply->format == 32);
    REQUIRE(reply->bytes_after == 0);
    auto count = xcb_get_property_value_length(reply.get()) / 4;
    auto* values = static_cast<uint32_t*>(xcb_get_property_value(reply.get()));
    return std::vector<uint32_t>(values, values + count);
}

inline std::vector<xcb_atom_t> get_window_property_atoms(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto values = read_property32(conn, window, atom, XCB_ATOM_ATOM);
    REQUIRE(values);
    return *values;
}

inline std::vector<xcb_window_t>
get_window_property_windows(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto values = read_property32(conn, window, atom, XCB_ATOM_WINDOW);
    REQUIRE(values);
    return *values;
}

inline bool property_has_atom(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t property, xcb_atom_t atom)
{
    auto atoms = read_property32(conn, window, property, XCB_ATOM_ATOM);
    return atoms && std::ranges::find(*atoms, atom) != atoms->end();
}

inline bool has_state(X11Connection& conn, xcb_window_t window, xcb_atom_t state)
{
    return property_has_atom(conn.get(), window, intern_atom(conn.get(), "_NET_WM_STATE"), state);
}

// Readiness probes may follow a supporting-window ID destroyed during restart.
inline std::optional<xcb_window_t>
get_window_property_window(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_ATOM_WINDOW, 0, 1);
    auto* reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply)
        return std::nullopt;

    std::optional<xcb_window_t> result;
    if (reply->type == XCB_ATOM_WINDOW && reply->format == 32 && xcb_get_property_value_length(reply) == 4)
    {
        result = *static_cast<xcb_window_t*>(xcb_get_property_value(reply));
    }
    free(reply);
    return result;
}

inline bool wait_for_property_window(
    xcb_connection_t* conn,
    xcb_window_t window,
    xcb_atom_t atom,
    xcb_window_t expected,
    std::chrono::milliseconds timeout
)
{
    return wait_for_condition(
        [conn, window, atom, expected]()
        {
            auto value = get_window_property_window(conn, window, atom);
            return value && *value == expected;
        },
        timeout
    );
}

inline bool wait_for_property_window_nonzero(
    xcb_connection_t* conn,
    xcb_window_t window,
    xcb_atom_t atom,
    std::chrono::milliseconds timeout
)
{
    return wait_for_condition(
        [conn, window, atom]()
        {
            auto value = get_window_property_window(conn, window, atom);
            return value && *value != XCB_NONE;
        },
        timeout
    );
}

inline std::optional<xcb_window_t> supporting_wm_window(X11Connection& conn)
{
    auto atom = intern_atom(conn.get(), "_NET_SUPPORTING_WM_CHECK");
    return get_window_property_window(conn.get(), conn.root(), atom);
}

inline bool wait_for_wm_ready(X11Connection& conn, std::chrono::milliseconds timeout, xcb_window_t previous = XCB_NONE)
{
    return wait_for_condition(
        [&]()
        {
            auto current = supporting_wm_window(conn);
            if (!current || *current == XCB_NONE || *current == previous)
                return false;
            auto atom = intern_atom(conn.get(), "_NET_SUPPORTING_WM_CHECK");
            return get_window_property_window(conn.get(), *current, atom) == current;
        },
        timeout
    );
}

inline xcb_window_t create_window(X11Connection& conn, int16_t x, int16_t y, uint16_t width, uint16_t height)
{
    xcb_window_t window = xcb_generate_id(conn.get());
    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t values[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_create_window(
        conn.get(),
        XCB_COPY_FROM_PARENT,
        window,
        conn.root(),
        x,
        y,
        width,
        height,
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        conn.screen()->root_visual,
        mask,
        values
    );
    return window;
}

inline bool set_window_type(X11Connection& conn, xcb_window_t window, xcb_atom_t type_atom)
{
    xcb_atom_t type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE");
    if (type == XCB_NONE)
        return false;
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, type, XCB_ATOM_ATOM, 32, 1, &type_atom);
    return true;
}

inline void map_window(X11Connection& conn, xcb_window_t window)
{
    xcb_map_window(conn.get(), window);
    xcb_flush(conn.get());
}

inline void set_window_wm_class(
    X11Connection& conn,
    xcb_window_t window,
    std::string const& instance_name,
    std::string const& class_name
)
{
    std::string value;
    value.reserve(instance_name.size() + class_name.size() + 2);
    value.append(instance_name);
    value.push_back('\0');
    value.append(class_name);
    value.push_back('\0');

    xcb_icccm_set_wm_class(conn.get(), window, static_cast<uint32_t>(value.size()), value.c_str());
    xcb_flush(conn.get());
}

inline void destroy_window(X11Connection& conn, xcb_window_t window)
{
    xcb_destroy_window(conn.get(), window);
    xcb_flush(conn.get());
}

inline std::optional<uint32_t>
get_window_property_cardinal(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto values = read_property32(conn, window, atom, XCB_ATOM_CARDINAL);
    if (!values)
        return std::nullopt;
    REQUIRE(values->size() == 1);
    return values->front();
}

inline uint32_t require_property_cardinal(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto value = get_window_property_cardinal(conn, window, atom);
    REQUIRE(value);
    return *value;
}

inline bool wait_for_property_cardinal(
    xcb_connection_t* conn,
    xcb_window_t window,
    xcb_atom_t atom,
    uint32_t expected,
    std::chrono::milliseconds timeout
)
{
    return wait_for_condition(
        [conn, window, atom, expected]()
        {
            auto value = get_window_property_cardinal(conn, window, atom);
            return value && *value == expected;
        },
        timeout
    );
}

inline std::optional<std::string>
get_window_property_string(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 4096);
    std::unique_ptr<xcb_get_property_reply_t, decltype(&free)> reply(
        xcb_get_property_reply(conn, cookie, nullptr),
        &free
    );
    REQUIRE(reply);
    if (reply->type == XCB_NONE)
        return std::nullopt;
    REQUIRE(reply->format == 8);
    REQUIRE((reply->type == XCB_ATOM_STRING || reply->type == intern_atom(conn, "UTF8_STRING")));
    REQUIRE(reply->bytes_after == 0);
    auto const* data = static_cast<char const*>(xcb_get_property_value(reply.get()));
    return std::string(data, xcb_get_property_value_length(reply.get()));
}

inline std::vector<std::string>
get_window_property_strings(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto raw = get_window_property_string(conn, window, atom);
    if (!raw)
        return { };

    std::vector<std::string> result;
    std::string current;
    for (char ch : *raw)
    {
        if (ch == '\0')
        {
            result.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }

    if (!current.empty())
        result.push_back(std::move(current));

    return result;
}

inline bool wait_for_property_strings(
    xcb_connection_t* conn,
    xcb_window_t window,
    xcb_atom_t atom,
    std::vector<std::string> expected,
    std::chrono::milliseconds timeout
)
{
    return wait_for_condition(
        [conn, window, atom, expected = std::move(expected)]()
        { return get_window_property_strings(conn, window, atom) == expected; },
        timeout
    );
}

inline void send_client_message(
    X11Connection& conn_wrapper,
    xcb_window_t target,
    xcb_atom_t type,
    uint32_t d0,
    uint32_t d1 = 0,
    uint32_t d2 = 0,
    uint32_t d3 = 0,
    uint32_t d4 = 0
)
{
    xcb_connection_t* conn = conn_wrapper.get();
    xcb_client_message_event_t event{};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.window = target;
    event.type = type;
    event.format = 32;
    event.data.data32[0] = d0;
    event.data.data32[1] = d1;
    event.data.data32[2] = d2;
    event.data.data32[3] = d3;
    event.data.data32[4] = d4;
    xcb_send_event(
        conn,
        0,
        conn_wrapper.root(),
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
        reinterpret_cast<char*>(&event)
    );
    xcb_flush(conn);
}

inline bool wait_for_active_window(X11Connection& conn, xcb_window_t expected, std::chrono::milliseconds timeout)
{
    xcb_atom_t active = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    if (active == XCB_NONE)
        return false;
    return wait_for_property_window(conn.get(), conn.root(), active, expected, timeout);
}

inline std::string make_temp_dir()
{
    std::filesystem::path base = std::filesystem::temp_directory_path() / "lwm-test-XXXXXX";
    std::string tmpl = base.string();
    std::vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');
    char* result = mkdtemp(buffer.data());
    if (!result)
        return "";
    return std::string(result);
}

inline std::optional<std::string> wait_for_ipc_socket_path(X11Connection& conn)
{
    auto atom = intern_atom(conn.get(), "_LWM_IPC_SOCKET");
    std::optional<std::string> path;
    wait_for_condition(
        [&]
        {
            path = get_window_property_string(conn.get(), conn.root(), atom);
            return path && !path->empty();
        },
        std::chrono::seconds(2)
    );
    return path;
}

inline std::optional<std::string> send_raw_ipc(std::string const& path, std::string const& command)
{
    if (path.size() >= sizeof(sockaddr_un::sun_path))
        return std::nullopt;
    struct Socket
    {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        ~Socket()
        {
            if (fd >= 0)
                close(fd);
        }
    } connection;
    int fd = connection.fd;
    if (fd < 0)
        return std::nullopt;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto ready = [&](short events)
    {
        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd descriptor{ fd, events, 0 };
        return remaining.count() > 0 && poll(&descriptor, 1, remaining.count()) > 0;
    };
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        return std::nullopt;
    std::string request = command + '\n';
    size_t sent = 0;
    while (sent < request.size())
    {
        if (!ready(POLLOUT))
            return std::nullopt;
        auto n = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n > 0)
            sent += n;
        else if (errno != EINTR && errno != EAGAIN)
            return std::nullopt;
    }
    shutdown(fd, SHUT_WR);
    std::string response;
    while (ready(POLLIN))
    {
        char buffer[4096];
        auto n = recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0)
            return response;
        if (n > 0)
            response.append(buffer, n);
        else if (errno != EINTR && errno != EAGAIN)
            return std::nullopt;
    }
    return std::nullopt;
}

inline std::optional<std::string> send_ipc_command(std::string const& path, std::string const& command)
{
    auto response = send_raw_ipc(path, command);
    if (response)
        while (!response->empty() && (response->back() == '\n' || response->back() == '\r' || response->back() == ' '))
            response->pop_back();
    return response;
}

inline std::filesystem::path find_test_executable_path(std::string_view name)
{
    if (name == "lwm")
        return LWM_BINARY_PATH;
    if (name == "lwmctl")
        return LWMCTL_BINARY_PATH;
    return {};
}

inline bool write_text_file(std::filesystem::path const& path, std::string_view content)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return false;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

inline std::string read_text_file(std::filesystem::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return "";

    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct CommandResult
{
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

inline std::optional<CommandResult> run_command(
    std::filesystem::path const& executable,
    std::vector<std::string> const& args,
    std::vector<std::pair<std::string, std::string>> const& env_overrides = { },
    std::vector<std::string> const& unset_env = { }
)
{
    if (!std::filesystem::exists(executable))
        return std::nullopt;

    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };
    if (pipe2(stdout_pipe, O_CLOEXEC) != 0 || pipe2(stderr_pipe, O_CLOEXEC) != 0)
    {
        if (stdout_pipe[0] != -1)
        {
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
        }
        if (stderr_pipe[0] != -1)
        {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        return std::nullopt;
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        for (auto const& key : unset_env) unsetenv(key.c_str());
        for (auto const& [key, value] : env_overrides)
        {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<std::string> owned_args;
        owned_args.reserve(args.size() + 1);
        owned_args.push_back(executable.string());
        owned_args.insert(owned_args.end(), args.begin(), args.end());

        std::vector<char*> argv;
        argv.reserve(owned_args.size() + 1);
        for (auto& arg : owned_args) argv.push_back(arg.data());
        argv.push_back(nullptr);

        execv(executable.c_str(), argv.data());
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    if (pid < 0)
    {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        return std::nullopt;
    }
    CommandResult result;
    std::array<pollfd, 2> fds{
        { { stdout_pipe[0], POLLIN, 0 }, { stderr_pipe[0], POLLIN, 0 } }
    };
    for (auto& fd : fds) fcntl(fd.fd, F_SETFL, fcntl(fd.fd, F_GETFL) | O_NONBLOCK);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status = 0;
    bool exited = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        poll(fds.data(), fds.size(), 10);
        for (size_t i = 0; i < fds.size(); ++i)
        {
            auto& fd = fds[i];
            if (fd.fd < 0)
                continue;
            std::array<char, 4096> buffer;
            ssize_t count = read(fd.fd, buffer.data(), buffer.size());
            if (count > 0)
                (i == 0 ? result.stdout_text : result.stderr_text).append(buffer.data(), count);
            else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            {
                close(fd.fd);
                fd.fd = -1;
            }
        }
        if (!exited)
            exited = waitpid(pid, &status, WNOHANG) == pid;
        if (exited && fds[0].fd < 0 && fds[1].fd < 0)
            break;
    }
    bool complete = exited && fds[0].fd < 0 && fds[1].fd < 0;
    for (auto& fd : fds)
        if (fd.fd >= 0)
            close(fd.fd);
    if (!exited)
    {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        { }
    }
    if (!complete)
    {
        result.stderr_text += "\nTest command timed out";
        return result;
    }
    if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exit_code = 128 + WTERMSIG(status);

    return result;
}

class LwmProcess
{
public:
    explicit LwmProcess(
        std::string display,
        std::string config_contents = {},
        std::vector<std::string> startup_args = {},
        int stderr_fd = -1
    )
        : display_(std::move(display))
        , config_home_(make_temp_dir())
        , runtime_dir_(make_temp_dir())
    {
        std::filesystem::path executable = find_test_executable_path("lwm");
        if (executable.empty())
            return;

        if (!config_contents.empty() && !write_config(config_contents))
            return;

        pid_ = fork();
        if (pid_ == 0)
        {
            int diagnostics = stderr_fd >= 0 ? dup(stderr_fd)
                : open((std::filesystem::path(runtime_dir_) / "stderr").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (diagnostics >= 0)
            {
                dup2(diagnostics, STDERR_FILENO);
                close(diagnostics);
            }
            unsetenv("LWM_SOCKET");
            if (!display_.empty())
                setenv("DISPLAY", display_.c_str(), 1);
            if (!config_home_.empty())
                setenv("XDG_CONFIG_HOME", config_home_.c_str(), 1);
            if (!runtime_dir_.empty())
                setenv("XDG_RUNTIME_DIR", runtime_dir_.c_str(), 1);

            std::vector<std::string> owned_args;
            owned_args.reserve(startup_args.size() + 1);
            owned_args.push_back(executable.string());
            owned_args.insert(owned_args.end(), { "--log-target", "stderr" });
            owned_args.insert(owned_args.end(), startup_args.begin(), startup_args.end());
            std::vector<char*> argv;
            argv.reserve(owned_args.size() + 1);
            for (std::string& arg : owned_args) argv.push_back(arg.data());
            argv.push_back(nullptr);
            execv(executable.c_str(), argv.data());
            _exit(127);
        }
        if (pid_ < 0)
            pid_ = -1;
    }

    ~LwmProcess()
    {
        stop();
        if (!config_home_.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(config_home_, ec);
        }
        if (!runtime_dir_.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(runtime_dir_, ec);
        }
    }

    LwmProcess(LwmProcess const&) = delete;
    LwmProcess& operator=(LwmProcess const&) = delete;

    LwmProcess(LwmProcess&& other) noexcept
        : pid_(other.pid_)
        , display_(std::move(other.display_))
        , config_home_(std::move(other.config_home_))
        , runtime_dir_(std::move(other.runtime_dir_))
    {
        other.pid_ = -1;
        other.config_home_.clear();
        other.runtime_dir_.clear();
    }

    LwmProcess& operator=(LwmProcess&& other) noexcept
    {
        if (this == &other)
            return *this;

        stop();

        if (!config_home_.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(config_home_, ec);
        }
        if (!runtime_dir_.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(runtime_dir_, ec);
        }

        pid_ = other.pid_;
        display_ = std::move(other.display_);
        config_home_ = std::move(other.config_home_);
        runtime_dir_ = std::move(other.runtime_dir_);
        other.pid_ = -1;
        other.config_home_.clear();
        other.runtime_dir_.clear();
        return *this;
    }

    bool running() const
    {
        if (pid_ <= 0)
            return false;
        siginfo_t info{};
        return waitid(P_PID, pid_, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == 0;
    }
    std::string diagnostics() const
    {
        return read_text_file(std::filesystem::path(runtime_dir_) / "stderr");
    }
    pid_t pid() const { return pid_; }
    std::filesystem::path config_path() const { return std::filesystem::path(config_home_) / "lwm" / "config.toml"; }
    std::string const& runtime_dir() const { return runtime_dir_; }
    std::string const& display() const { return display_; }

    bool write_config(std::string_view content) const { return write_text_file(config_path(), content); }

    std::optional<int> wait_for_exit(std::chrono::milliseconds timeout)
    {
        if (pid_ <= 0)
            return std::nullopt;
        int status = 0;
        bool exited = wait_for_condition([&] { return waitpid(pid_, &status, WNOHANG) == pid_; }, timeout);
        if (!exited)
            return std::nullopt;
        pid_ = -1;
        return status;
    }

    void stop()
    {
        if (pid_ <= 0)
            return;

        kill(pid_, SIGTERM);
        bool exited = wait_for_exit(std::chrono::milliseconds(1000)).has_value();
        if (!exited)
        {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
        pid_ = -1;
    }

private:
    pid_t pid_ = -1;
    std::string display_;
    std::string config_home_;
    std::string runtime_dir_;
};

// Only missing optional infrastructure may skip; a broken WM must fail.
struct TestEnvironment
{
    X11TestEnvironment& x11_env;
    X11Connection conn;
    LwmProcess wm;

    static std::optional<TestEnvironment> create(std::string config = {})
    {
        auto& env = X11TestEnvironment::instance();
        if (!env.available())
        {
            REQUIRE(std::getenv("LWM_TEST_REQUIRE_X11") == nullptr);
            return std::nullopt;
        }
        X11Connection conn;
        REQUIRE(conn.ok());
        LwmProcess wm(env.display(), std::move(config));
        auto socket_atom = intern_atom(conn.get(), "_LWM_IPC_SOCKET");
        bool ready = wait_for_condition(
            [&]
            {
                if (!wm.running())
                    return true;
                auto path = get_window_property_string(conn.get(), conn.root(), socket_atom);
                // A previous WM can leave root properties behind on a reused server.
                return path && path->starts_with(wm.runtime_dir() + "/") && send_ipc_command(*path, "ping") == "ok pong"
                    && wait_for_wm_ready(conn, std::chrono::milliseconds(10));
            },
            std::chrono::seconds(2)
        );
        INFO(wm.diagnostics());
        REQUIRE(wm.running());
        REQUIRE(ready);
        return TestEnvironment{ env, std::move(conn), std::move(wm) };
    }
};

inline std::filesystem::path lwmctl_executable_path()
{
    auto path = find_test_executable_path("lwmctl");
    REQUIRE(std::filesystem::exists(path));
    return path;
}

inline std::optional<CommandResult>
run_lwmctl(LwmProcess const& wm, std::vector<std::string> const& args, std::string socket_path = {})
{
    std::vector<std::pair<std::string, std::string>> env = {
        {         "DISPLAY",     wm.display() },
        { "XDG_RUNTIME_DIR", wm.runtime_dir() },
    };
    if (!socket_path.empty())
        env.emplace_back("LWM_SOCKET", std::move(socket_path));

    return run_command(lwmctl_executable_path(), args, env, { "LWM_SOCKET" });
}

} // namespace lwm::test
