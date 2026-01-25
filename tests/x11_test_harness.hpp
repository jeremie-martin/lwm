#pragma once

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>

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

    bool available() const { return available_; }
    std::string const& display() const { return display_; }

private:
    X11TestEnvironment()
    {
        char const* allow_existing = std::getenv("LWM_TEST_ALLOW_EXISTING_DISPLAY");
        bool use_existing = allow_existing && std::strcmp(allow_existing, "1") == 0;

        if (start_xvfb())
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
        stop_xvfb();
        restore_display();
    }

    X11TestEnvironment(X11TestEnvironment const&) = delete;
    X11TestEnvironment& operator=(X11TestEnvironment const&) = delete;

    bool start_xvfb()
    {
        auto xvfb = find_in_path("Xvfb");
        if (!xvfb)
            return false;

        char const* previous = std::getenv("DISPLAY");
        if (previous && *previous)
            previous_display_ = std::string(previous);

        for (int display_num = 99; display_num <= 120; ++display_num)
        {
            std::string display = ":" + std::to_string(display_num);
            pid_t pid = fork();
            if (pid == 0)
            {
                execl(
                    xvfb->c_str(),
                    "Xvfb",
                    display.c_str(),
                    "-screen",
                    "0",
                    "1280x720x24",
                    "-nolisten",
                    "tcp",
                    nullptr
                );
                _exit(127);
            }
            if (pid < 0)
                continue;

            setenv("DISPLAY", display.c_str(), 1);
            display_modified_ = true;
            if (wait_for_x_server(std::chrono::milliseconds(1000)))
            {
                display_ = display;
                xvfb_pid_ = pid;
                owns_display_ = true;
                return true;
            }

            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }

        restore_display();
        return false;
    }

    bool wait_for_x_server(std::chrono::milliseconds timeout)
    {
        return wait_for_condition(
            []()
            {
                xcb_connection_t* conn = xcb_connect(nullptr, nullptr);
                bool ok = conn && !xcb_connection_has_error(conn);
                if (conn)
                    xcb_disconnect(conn);
                return ok;
            },
            timeout
        );
    }

    void stop_xvfb()
    {
        if (!owns_display_ || xvfb_pid_ <= 0)
            return;

        kill(xvfb_pid_, SIGTERM);
        bool exited = wait_for_condition(
            [this]() { return waitpid(xvfb_pid_, nullptr, WNOHANG) > 0; },
            std::chrono::milliseconds(1000)
        );
        if (!exited)
        {
            kill(xvfb_pid_, SIGKILL);
            waitpid(xvfb_pid_, nullptr, 0);
        }
        xvfb_pid_ = -1;
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
    pid_t xvfb_pid_ = -1;
    std::string display_;
    std::optional<std::string> previous_display_;
    bool display_modified_ = false;
};

class X11Connection
{
public:
    X11Connection()
        : conn_(xcb_connect(nullptr, nullptr))
        , screen_(nullptr)
    {
        if (conn_ && !xcb_connection_has_error(conn_))
        {
            screen_ = xcb_setup_roots_iterator(xcb_get_setup(conn_)).data;
        }
    }

    ~X11Connection()
    {
        if (conn_)
            xcb_disconnect(conn_);
    }

    bool ok() const { return conn_ && !xcb_connection_has_error(conn_) && screen_; }
    xcb_connection_t* get() const { return conn_; }
    xcb_screen_t* screen() const { return screen_; }
    xcb_window_t root() const { return screen_->root; }

private:
    xcb_connection_t* conn_;
    xcb_screen_t* screen_;
};

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

inline std::optional<xcb_window_t>
get_window_property_window(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_ATOM_WINDOW, 0, 1);
    auto* reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply)
        return std::nullopt;

    std::optional<xcb_window_t> result;
    if (xcb_get_property_value_length(reply) >= 4)
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

inline bool wait_for_wm_ready(X11Connection& conn, std::chrono::milliseconds timeout)
{
    xcb_atom_t supporting = intern_atom(conn.get(), "_NET_SUPPORTING_WM_CHECK");
    if (supporting == XCB_NONE)
        return false;
    return wait_for_property_window_nonzero(conn.get(), conn.root(), supporting, timeout);
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

inline void set_window_type(X11Connection& conn, xcb_window_t window, xcb_atom_t type_atom)
{
    xcb_atom_t type = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE");
    if (type == XCB_NONE)
        return;
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, type, XCB_ATOM_ATOM, 32, 1, &type_atom);
}

inline void map_window(X11Connection& conn, xcb_window_t window)
{
    xcb_map_window(conn.get(), window);
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
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_ATOM_CARDINAL, 0, 1);
    auto* reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply)
        return std::nullopt;

    std::optional<uint32_t> result;
    if (xcb_get_property_value_length(reply) >= 4)
    {
        result = *static_cast<uint32_t*>(xcb_get_property_value(reply));
    }
    free(reply);
    return result;
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

class LwmProcess
{
public:
    explicit LwmProcess(std::string display)
        : display_(std::move(display))
        , config_home_(make_temp_dir())
    {
        std::filesystem::path executable = std::filesystem::current_path() / "src" / "app" / "lwm";
        if (!std::filesystem::exists(executable))
            return;

        pid_ = fork();
        if (pid_ == 0)
        {
            if (!display_.empty())
                setenv("DISPLAY", display_.c_str(), 1);
            if (!config_home_.empty())
                setenv("XDG_CONFIG_HOME", config_home_.c_str(), 1);
            execl(executable.c_str(), executable.c_str(), nullptr);
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
    }

    bool running() const { return pid_ > 0; }

    void stop()
    {
        if (pid_ <= 0)
            return;

        kill(pid_, SIGTERM);
        bool exited = wait_for_condition(
            [this]() { return waitpid(pid_, nullptr, WNOHANG) > 0; },
            std::chrono::milliseconds(1000)
        );
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
};

} // namespace lwm::test
