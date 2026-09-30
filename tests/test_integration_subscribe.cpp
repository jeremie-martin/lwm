#include "ipc_subscription.hpp"
#include <X11/Xlib.h>
#include <cstdio>
#include <memory>
#include <xcb/xcb_keysyms.h>
#include <xcb/xtest.h>

using namespace lwm::test;

namespace {
constexpr auto kTimeout = std::chrono::seconds(2);

// Only the consumer-close contract needs a CLI process. Protocol/content tests
// use Subscriber's acknowledged socket; CLI framing has its own independent peer.
struct CliSubscriber
{
    TestFd output;
    LineReader reader;
    std::unique_ptr<FILE, int (*)(FILE*)> errors{ tmpfile(), fclose };
    pid_t pid = -1;

    explicit CliSubscriber(LwmProcess const& wm)
    {
        REQUIRE(errors);
        REQUIRE(fcntl(fileno(errors.get()), F_SETFD, FD_CLOEXEC) == 0);
        int pipe[2];
        REQUIRE(pipe2(pipe, O_CLOEXEC) == 0);
        output.fd = pipe[0];
        TestFd writer{ pipe[1] };
        auto executable = lwmctl_executable_path();
        pid = fork();
        REQUIRE(pid >= 0);
        if (pid == 0)
        {
            if (dup2(writer.fd, STDOUT_FILENO) < 0 || dup2(fileno(errors.get()), STDERR_FILENO) < 0)
                _exit(126);
            output.reset();
            writer.reset();
            setenv("DISPLAY", wm.display().c_str(), 1);
            setenv("XDG_RUNTIME_DIR", wm.runtime_dir().c_str(), 1);
            execl(executable.c_str(), executable.c_str(), "subscribe", "window_map", nullptr);
            _exit(127);
        }
    }
    ~CliSubscriber()
    {
        if (pid > 0)
        {
            kill(pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
            { }
        }
    }
    bool wait(int& status)
    {
        return wait_for_condition(
            [&]
            {
                if (waitpid(pid, &status, WNOHANG) != pid)
                    return false;
                pid = -1;
                return true;
            },
            kTimeout
        );
    }
};

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

} // namespace

TEST_CASE("Integration: lwmctl subscribe exits when stdout consumer closes", "[integration][subscribe]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    CliSubscriber child(env->wm);

    // The CLI intentionally hides the acknowledgement. Generate real events
    // until one is delivered, rather than guessing when it has subscribed.
    std::vector<xcb_window_t> windows;
    auto deadline = std::chrono::steady_clock::now() + kTimeout;
    std::optional<std::string> first;
    while (!first && std::chrono::steady_clock::now() < deadline)
    {
        windows.push_back(create_window(conn, 10, 10, 200, 150));
        map_window(conn, windows.back());
        first = child.reader.read(child.output.fd, std::chrono::milliseconds(50));
    }
    REQUIRE(first);
    auto event = nlohmann::json::parse(*first);
    REQUIRE(event.at("event") == "window_map");
    REQUIRE(std::find(windows.begin(), windows.end(), event.at("window").get<xcb_window_t>()) != windows.end());

    child.output.reset();
    auto next = create_window(conn, 40, 40, 200, 150);
    map_window(conn, next);
    int status = 0;
    REQUIRE(child.wait(status));
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    rewind(child.errors.get());
    CHECK(fgetc(child.errors.get()) == EOF);
    destroy_window(conn, next);
    for (auto window : windows) destroy_window(conn, window);
}

TEST_CASE("Integration: subscription JSON preserves escaped focus fields", "[integration][subscribe][json]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto path = wait_for_ipc_socket_path(env->conn);
    REQUIRE(path);
    Subscriber subscriber(*path, "focus_change");
    auto window = create_window(env->conn, 10, 10, 200, 150);
    set_window_wm_class(env->conn, window, "escaped-instance", "escaped-class");
    std::string title = "quote \" and slash \\ line\n tab\t";
    set_window_title(env->conn, window, title);
    map_window(env->conn, window);
    auto event = subscriber.event();
    CHECK(event.at("event") == "focus_change");
    CHECK(event.at("window") == window);
    CHECK(event.at("class") == "escaped-class");
    CHECK(event.at("title") == title);
    destroy_window(env->conn, window);
}

TEST_CASE("Integration: subscription preserves monitor direction in key_action events", "[integration][subscribe]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    if (!extension_available(conn, &xcb_test_id))
        SKIP("XTEST extension not available");
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    Subscriber subscriber(*path, "key_action");
    REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("Left")));
    REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("Right")));
    for (auto action : { "focus_monitor_left", "focus_monitor_right" })
    {
        auto event = subscriber.event();
        CHECK(event.at("event") == "key_action");
        CHECK(event.at("action") == action);
    }
}
