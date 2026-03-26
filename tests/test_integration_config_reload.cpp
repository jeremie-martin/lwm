#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <sstream>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

struct TestEnvironment
{
    X11TestEnvironment& x11_env;
    X11Connection conn;
    LwmProcess wm;

    bool ok() const { return conn.ok() && wm.running(); }

    static std::optional<TestEnvironment> create(std::string const& config)
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

        LwmProcess wm(env.display(), config);
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

std::string toml_escape(std::string const& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value)
    {
        switch (ch)
        {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

std::string make_config(
    std::string const& first_name,
    std::string const& second_name,
    std::optional<size_t> workspace_count = 2,
    std::string const& autostart_command = {},
    std::string const& extra = {}
)
{
    std::ostringstream out;
    out << "[appearance]\n";
    out << "padding = 10\n";
    out << "border_width = 2\n";
    out << "border_color = 0xFF0000\n\n";
    out << "[workspaces]\n";
    if (workspace_count.has_value())
        out << "count = " << *workspace_count << "\n";
    out << "names = [\"" << toml_escape(first_name) << "\", \"" << toml_escape(second_name) << "\"]\n";

    if (!autostart_command.empty())
    {
        out << "\n[autostart]\n";
        out << "commands = [\"" << toml_escape(autostart_command) << "\"]\n";
    }

    if (!extra.empty())
        out << "\n" << extra;

    return out.str();
}

std::optional<std::string> wait_for_ipc_socket_path(X11Connection& conn)
{
    xcb_atom_t socket_atom = intern_atom(conn.get(), "_LWM_IPC_SOCKET");
    if (socket_atom == XCB_NONE)
        return std::nullopt;

    bool ready = wait_for_condition(
        [&conn, socket_atom]()
        {
            auto value = get_window_property_string(conn.get(), conn.root(), socket_atom);
            return value && !value->empty();
        },
        kTimeout
    );
    if (!ready)
        return std::nullopt;

    return get_window_property_string(conn.get(), conn.root(), socket_atom);
}

std::vector<std::string> desktop_names(X11Connection& conn)
{
    xcb_atom_t names_atom = intern_atom(conn.get(), "_NET_DESKTOP_NAMES");
    if (names_atom == XCB_NONE)
        return {};
    return get_window_property_strings(conn.get(), conn.root(), names_atom);
}

bool wait_for_desktop_names(X11Connection& conn, std::vector<std::string> expected)
{
    xcb_atom_t names_atom = intern_atom(conn.get(), "_NET_DESKTOP_NAMES");
    if (names_atom == XCB_NONE)
        return false;
    return wait_for_property_strings(conn.get(), conn.root(), names_atom, std::move(expected), kTimeout);
}

size_t line_count(std::filesystem::path const& path)
{
    std::string contents = read_text_file(path);
    if (contents.empty())
        return 0;

    size_t count = 0;
    for (char ch : contents)
    {
        if (ch == '\n')
            ++count;
    }

    if (!contents.empty() && contents.back() != '\n')
        ++count;

    return count;
}

bool ensure_lwmctl_available()
{
    if (!std::filesystem::exists(lwmctl_executable_path()))
    {
        WARN("lwmctl binary not built.");
        return false;
    }
    return true;
}

bool wait_for_window_geometry(
    X11Connection& conn,
    xcb_window_t window,
    int16_t x,
    int16_t y,
    uint16_t width,
    uint16_t height
)
{
    return wait_for_condition(
        [&conn, window, x, y, width, height]()
        {
            auto cookie = xcb_get_geometry(conn.get(), window);
            auto* reply = xcb_get_geometry_reply(conn.get(), cookie, nullptr);
            if (!reply)
                return false;

            bool matches = reply->x == x && reply->y == y && reply->width == width && reply->height == height;
            free(reply);
            return matches;
        },
        kTimeout
    );
}

} // namespace

TEST_CASE("Integration: reload-config updates desktop names and keeps failed reload atomic", "[integration][ipc][reload]")
{
    auto env = TestEnvironment::create(make_config("dev", "web"));
    if (!env || !ensure_lwmctl_available())
        SKIP("Test environment or lwmctl not available");

    auto socket_path = wait_for_ipc_socket_path(env->conn);
    REQUIRE(socket_path.has_value());
    REQUIRE(wait_for_desktop_names(env->conn, { "dev", "web" }));

    REQUIRE(env->wm.write_config(make_config("code", "chat")));
    auto reload_ok = run_lwmctl(env->wm, { "reload-config" }, *socket_path);
    REQUIRE(reload_ok.has_value());
    REQUIRE(reload_ok->exit_code == 0);
    REQUIRE(wait_for_desktop_names(env->conn, { "code", "chat" }));

    REQUIRE(env->wm.write_config("[workspaces]\ncount = 2\nnames = [\"broken\"\n"));
    auto reload_bad = run_lwmctl(env->wm, { "reload-config" }, *socket_path);
    REQUIRE(reload_bad.has_value());
    REQUIRE(reload_bad->exit_code != 0);
    REQUIRE(wait_for_desktop_names(env->conn, { "code", "chat" }));
}

TEST_CASE("Integration: reload-config rejects workspace-count changes", "[integration][ipc][reload]")
{
    auto env = TestEnvironment::create(make_config("one", "two"));
    if (!env || !ensure_lwmctl_available())
        SKIP("Test environment or lwmctl not available");

    auto socket_path = wait_for_ipc_socket_path(env->conn);
    REQUIRE(socket_path.has_value());

    xcb_atom_t desktops_atom = intern_atom(env->conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    REQUIRE(desktops_atom != XCB_NONE);
    REQUIRE(wait_for_property_cardinal(env->conn.get(), env->conn.root(), desktops_atom, 2, kTimeout));

    REQUIRE(env->wm.write_config(make_config("one", "two", 3)));
    auto reload_result = run_lwmctl(env->wm, { "reload-config" }, *socket_path);
    REQUIRE(reload_result.has_value());
    REQUIRE(reload_result->exit_code != 0);

    REQUIRE(wait_for_property_cardinal(env->conn.get(), env->conn.root(), desktops_atom, 2, kTimeout));
    REQUIRE(wait_for_desktop_names(env->conn, { "one", "two" }));
}

TEST_CASE("Integration: IPC ping and reload succeed", "[integration][ipc][reload]")
{
    auto env = TestEnvironment::create(make_config("left", "right"));
    if (!env || !ensure_lwmctl_available())
        SKIP("Test environment or lwmctl not available");

    auto socket_path = wait_for_ipc_socket_path(env->conn);
    REQUIRE(socket_path.has_value());

    auto ping_result = run_lwmctl(env->wm, { "ping" }, *socket_path);
    REQUIRE(ping_result.has_value());
    REQUIRE(ping_result->exit_code == 0);

    auto reload_result = run_lwmctl(env->wm, { "reload-config" }, *socket_path);
    REQUIRE(reload_result.has_value());
    REQUIRE(reload_result->exit_code == 0);
}

TEST_CASE("Integration: reload-config does not rerun autostart", "[integration][ipc][reload][autostart]")
{
    auto marker_dir = make_temp_dir();
    if (marker_dir.empty())
        SKIP("Failed to create marker directory");

    std::filesystem::path marker_path = std::filesystem::path(marker_dir) / "autostart.log";
    std::string autostart_cmd = "sh -c 'echo start >> " + marker_path.string() + "'";

    auto env = TestEnvironment::create(make_config("alpha", "beta", 2, autostart_cmd));
    if (!env || !ensure_lwmctl_available())
        SKIP("Test environment or lwmctl not available");

    auto socket_path = wait_for_ipc_socket_path(env->conn);
    REQUIRE(socket_path.has_value());

    REQUIRE(wait_for_condition(
        [&marker_path]() { return std::filesystem::exists(marker_path) && line_count(marker_path) == 1; },
        kTimeout
    ));

    REQUIRE(env->wm.write_config(make_config("gamma", "delta", 2, autostart_cmd)));
    auto reload_result = run_lwmctl(env->wm, { "reload-config" }, *socket_path);
    REQUIRE(reload_result.has_value());
    REQUIRE(reload_result->exit_code == 0);
    REQUIRE(wait_for_desktop_names(env->conn, { "gamma", "delta" }));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(line_count(marker_path) == 1);

    std::error_code ec;
    std::filesystem::remove_all(marker_dir, ec);
}

TEST_CASE("Integration: reload-config reapplies geometry rules to visible floating windows", "[integration][ipc][reload][rules]")
{
    auto env = TestEnvironment::create(make_config("left", "right"));
    if (!env || !ensure_lwmctl_available())
        SKIP("Test environment or lwmctl not available");

    auto socket_path = wait_for_ipc_socket_path(env->conn);
    REQUIRE(socket_path.has_value());

    xcb_atom_t dialog_type = intern_atom(env->conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    REQUIRE(dialog_type != XCB_NONE);

    xcb_window_t floating = create_window(env->conn, 40, 40, 120, 90);
    set_window_type(env->conn, floating, dialog_type);
    map_window(env->conn, floating);
    REQUIRE(wait_for_active_window(env->conn, floating, kTimeout));

    std::string rules = R"(
[[rules]]
type = "dialog"
geometry = { x = 300, y = 200, width = 240, height = 160 }
)";
    REQUIRE(env->wm.write_config(make_config("left", "right", 2, {}, rules)));

    auto reload_result = run_lwmctl(env->wm, { "reload-config" }, *socket_path);
    REQUIRE(reload_result.has_value());
    REQUIRE(reload_result->exit_code == 0);
    REQUIRE(wait_for_window_geometry(env->conn, floating, 300, 200, 240, 160));

    destroy_window(env->conn, floating);
}
