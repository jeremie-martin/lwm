#include "x11_test_harness.hpp"
#include <X11/keysym.h>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

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
        out << "commands = [{ shell = \"" << toml_escape(autostart_command) << "\" }]\n";
    }

    if (!extra.empty())
        out << "\n" << extra;

    return out.str();
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

// A shell whose command line names `needle` is found until its commands have finished.
bool process_running_with_argument(std::string const& needle)
{
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator("/proc", ec))
    {
        if (read_text_file(entry.path() / "cmdline").find(needle) != std::string::npos)
            return true;
    }
    return false;
}

std::optional<std::pair<int16_t, int16_t>> window_center(X11Connection& conn, xcb_window_t window)
{
    auto cookie = xcb_get_geometry(conn.get(), window);
    auto* reply = xcb_get_geometry_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return std::nullopt;

    auto center = std::pair<int16_t, int16_t>{
        static_cast<int16_t>(reply->x + reply->width / 2),
        static_cast<int16_t>(reply->y + reply->height / 2),
    };
    free(reply);
    return center;
}

} // namespace

TEST_CASE(
    "Integration: reload-config updates desktop names and keeps failed reload atomic",
    "[integration][ipc][reload]"
)
{
    auto env = TestEnvironment::create(make_config("dev", "web"));
    if (!env)
        SKIP("Test environment not available");

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
    if (!env)
        SKIP("Test environment not available");

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

TEST_CASE("Integration: reload-config does not rerun autostart", "[integration][ipc][reload][autostart]")
{
    auto marker_dir = make_temp_dir();
    REQUIRE_FALSE(marker_dir.empty());

    std::filesystem::path marker_path = std::filesystem::path(marker_dir) / "autostart.log";
    std::string autostart_cmd = "sh -c 'echo start >> " + marker_path.string() + "'";

    auto env = TestEnvironment::create(make_config("alpha", "beta", 2, autostart_cmd));
    if (!env)
        SKIP("Test environment not available");

    auto socket_path = wait_for_ipc_socket_path(env->conn);
    REQUIRE(socket_path.has_value());

    REQUIRE(wait_for_condition(
        [&marker_path]() { return std::filesystem::exists(marker_path) && line_count(marker_path) == 1; },
        kTimeout
    ));
    REQUIRE(wait_for_condition([&] { return !process_running_with_argument(marker_path.string()); }, kTimeout));

    REQUIRE(env->wm.write_config(make_config("gamma", "delta", 2, autostart_cmd)));
    auto reload_result = run_lwmctl(env->wm, { "reload-config" }, *socket_path);
    REQUIRE(reload_result.has_value());
    REQUIRE(reload_result->exit_code == 0);
    REQUIRE(wait_for_desktop_names(env->conn, { "gamma", "delta" }));

    // Launching returns only after exec, so a rerun during reload would have a shell by now.
    // Check that no such shell is still running before checking that none appended a line.
    REQUIRE_FALSE(process_running_with_argument(marker_path.string()));
    REQUIRE(line_count(marker_path) == 1);

    std::error_code ec;
    std::filesystem::remove_all(marker_dir, ec);
}

TEST_CASE(
    "Integration: reload-config reapplies geometry rules to visible floating windows",
    "[integration][ipc][reload][rules]"
)
{
    auto env = TestEnvironment::create(make_config("left", "right"));
    if (!env)
        SKIP("Test environment not available");

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
match = { type = "dialog" }
apply = { geometry = { x = 300, y = 200, width = 240, height = 160 } }
)";
    REQUIRE(env->wm.write_config(make_config("left", "right", 2, {}, rules)));

    auto reload_result = run_lwmctl(env->wm, { "reload-config" }, *socket_path);
    REQUIRE(reload_result.has_value());
    REQUIRE(reload_result->exit_code == 0);
    REQUIRE(wait_for_window_geometry(env->conn, floating, 300, 200, 240, 160));

    destroy_window(env->conn, floating);
}

TEST_CASE(
    "Integration: reload-config reapplies workspace rules to existing windows",
    "[integration][ipc][reload][rules][workspace]"
)
{
    auto env = TestEnvironment::create(make_config("left", "right"));
    if (!env)
        SKIP("Test environment not available");

    auto socket_path = wait_for_ipc_socket_path(env->conn);
    REQUIRE(socket_path.has_value());

    xcb_atom_t net_current_desktop = intern_atom(env->conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(env->conn.get(), "_NET_WM_DESKTOP");
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);

    xcb_window_t fallback = create_window(env->conn, 10, 10, 120, 90);
    map_window(env->conn, fallback);
    REQUIRE(wait_for_active_window(env->conn, fallback, kTimeout));

    xcb_window_t window = create_window(env->conn, 40, 40, 120, 90);
    set_window_title(env->conn, window, "reload-move");
    map_window(env->conn, window);
    REQUIRE(wait_for_active_window(env->conn, window, kTimeout));
    REQUIRE(wait_for_property_cardinal(env->conn.get(), window, net_wm_desktop, 0, kTimeout));

    std::string rules = R"(
[[rules]]
match = { title = "reload-move" }
apply = { workspace = 1 }
)";
    REQUIRE(env->wm.write_config(make_config("left", "right", 2, {}, rules)));

    auto reload_result = run_lwmctl(env->wm, { "reload-config" }, *socket_path);
    REQUIRE(reload_result.has_value());
    REQUIRE(reload_result->exit_code == 0);
    REQUIRE(wait_for_property_cardinal(env->conn.get(), window, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_property_cardinal(env->conn.get(), env->conn.root(), net_current_desktop, 0, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(env->conn, window); }, kTimeout));
    REQUIRE(wait_for_active_window(env->conn, fallback, kTimeout));

    destroy_window(env->conn, window);
    destroy_window(env->conn, fallback);
}

TEST_CASE(
    "Integration: reload-config keeps fallback focus when removing a hidden scratchpad",
    "[integration][ipc][reload][scratchpad][focus]"
)
{
    std::string scratchpad = R"(
[[scratchpads]]
name = "scratchpad"
spawn = { argv = ["/bin/true"] }
match = { class = "ScratchpadClass", instance = "scratchpad-instance" }
size = { width = 0.8, height = 0.6 }
)";
    auto env = TestEnvironment::create(make_config("one", "two", 2, {}, scratchpad));
    if (!env)
        SKIP("Test environment not available");

    auto socket_path = wait_for_ipc_socket_path(env->conn);
    REQUIRE(socket_path.has_value());

    xcb_window_t fallback = create_window(env->conn, 10, 10, 400, 300);
    set_window_wm_class(env->conn, fallback, "fallback-instance", "FallbackClass");
    map_window(env->conn, fallback);
    REQUIRE(wait_for_active_window(env->conn, fallback, kTimeout));

    auto show_result = send_ipc_command(*socket_path, "scratchpad toggle scratchpad");
    REQUIRE(show_result.has_value());
    REQUIRE(*show_result == "ok");

    xcb_window_t scratchpad_window = create_window(env->conn, 10, 10, 240, 160);
    set_window_wm_class(env->conn, scratchpad_window, "scratchpad-instance", "ScratchpadClass");
    map_window(env->conn, scratchpad_window);
    REQUIRE(wait_for_active_window(env->conn, scratchpad_window, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(env->conn, scratchpad_window); }, kTimeout));

    auto saved_center = window_center(env->conn, scratchpad_window);
    REQUIRE(saved_center.has_value());

    auto hide_result = send_ipc_command(*socket_path, "scratchpad toggle scratchpad");
    REQUIRE(hide_result.has_value());
    REQUIRE(*hide_result == "ok");
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(env->conn, scratchpad_window); }, kTimeout));
    REQUIRE(wait_for_active_window(env->conn, fallback, kTimeout));

    xcb_warp_pointer(
        env->conn.get(),
        XCB_NONE,
        env->conn.root(),
        0,
        0,
        0,
        0,
        saved_center->first,
        saved_center->second
    );
    xcb_flush(env->conn.get());
    REQUIRE(wait_for_active_window(env->conn, fallback, kTimeout));

    REQUIRE(env->wm.write_config(make_config("one", "two")));
    auto reload_result = run_lwmctl(env->wm, { "reload-config" }, *socket_path);
    REQUIRE(reload_result.has_value());
    REQUIRE(reload_result->exit_code == 0);
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(env->conn, scratchpad_window); }, kTimeout));
    REQUIRE(wait_for_active_window(env->conn, fallback, kTimeout));

    destroy_window(env->conn, scratchpad_window);
    destroy_window(env->conn, fallback);
}

TEST_CASE("Integration: a reload binding can replace itself and publish new bindings", "[integration][reload][keybind]")
{
    auto env = TestEnvironment::create(R"(
[workspaces]
count = 2
names = ["before", "two"]
[[binds]]
key = "F5"
action = "reload-config"
[[binds]]
key = "F6"
action = "workspace switch 1"
)");
    REQUIRE(env);
    auto& conn = env->conn;
    auto desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    REQUIRE(send_key(conn, XK_F6));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), desktop, 1, kTimeout));
    REQUIRE(env->wm.write_config(R"(
[workspaces]
count = 2
names = ["after", "two"]
[[binds]]
key = "F6"
action = "workspace switch 0"
)"));
    REQUIRE(send_key(conn, XK_F5));
    REQUIRE(wait_for_desktop_names(conn, { "after", "two" }));
    REQUIRE(send_key(conn, XK_F6));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), desktop, 0, kTimeout));

    // Invalid replacement must leave both the published config and working bindings intact.
    REQUIRE(env->wm.write_config(R"(
[workspaces]
count = 2
names = ["invalid", "two"]
[[binds]]
key = "F6"
action = "workspace switch 1"
[[rules]]
match = { title = "[invalid" }
apply = { floating = true }
)"));
    auto result = run_lwmctl(env->wm, { "reload-config" });
    REQUIRE(result);
    REQUIRE(result->exit_code != 0);
    REQUIRE(wait_for_desktop_names(conn, { "after", "two" }));
    auto switched = run_lwmctl(env->wm, { "workspace", "switch", "1" });
    REQUIRE(switched);
    REQUIRE(switched->exit_code == 0);
    REQUIRE(send_key(conn, XK_F6));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), desktop, 0, kTimeout));
}

TEST_CASE(
    "Integration: reload preserves scratchpad claims and pending launches by name",
    "[integration][reload][scratchpad]"
)
{
    auto config = [](bool keep_claimed, std::string pattern)
    {
        std::string result = R"(
[workspaces]
count = 2
[[scratchpads]]
name = "pending"
spawn = { argv = ["/bin/true"] }
match = { class = "Pending" }
)";
        if (keep_claimed)
            result += "\n[[scratchpads]]\nname = \"claimed\"\nspawn = { argv = [\"/bin/true\"] }\nmatch = { class = \""
                + pattern + "\" }\n";
        return result;
    };
    auto env = TestEnvironment::create(config(true, "Original"));
    REQUIRE(env);
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto snapshot = [&]
    {
        auto reply = send_ipc_command(*socket, "scratchpad list");
        REQUIRE(reply);
        REQUIRE(reply->starts_with("ok "));
        return nlohmann::json::parse(reply->substr(3));
    };
    REQUIRE(send_ipc_command(*socket, "scratchpad toggle pending") == "ok");
    auto window = create_window(conn, 10, 10, 200, 150);
    set_window_wm_class(conn, window, "instance", "Original");
    map_window(conn, window);
    REQUIRE(wait_for_condition([&] { return snapshot()["named"][1]["window"] == window; }, kTimeout));
    auto before = snapshot();
    REQUIRE(before["named"][0]["pending"] == true);
    REQUIRE(env->wm.write_config(config(true, "[invalid")));
    auto rejected = run_lwmctl(env->wm, { "reload-config" });
    REQUIRE(rejected);
    REQUIRE(rejected->exit_code != 0);
    CHECK(snapshot() == before);

    REQUIRE(env->wm.write_config(config(true, "Replacement")));
    REQUIRE(send_ipc_command(*socket, "reload-config") == "ok reloaded");
    CHECK(snapshot() == before);
    REQUIRE(send_ipc_command(*socket, "scratchpad toggle claimed") == "ok");
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(send_ipc_command(*socket, "scratchpad toggle claimed") == "ok");
    auto hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    REQUIRE(wait_for_condition([&] { return has_state(conn, window, hidden); }, kTimeout));
    REQUIRE(env->wm.write_config(config(false, "")));
    REQUIRE(send_ipc_command(*socket, "reload-config") == "ok reloaded");
    auto remaining = snapshot();
    REQUIRE(remaining["named"].size() == 1);
    CHECK(remaining["named"][0]["pending"] == true);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(wait_for_condition([&] { return !has_state(conn, window, hidden); }, kTimeout));
}

TEST_CASE("Integration: resolved launch bindings preserve argv and replace references on reload", "[integration][reload][keybind][spawn]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto socket = wait_for_ipc_socket_path(env->conn);
    REQUIRE(socket);
    auto literal_path = std::filesystem::path(env->wm.runtime_dir()) / "literal output";
    auto shell_path = std::filesystem::path(env->wm.runtime_dir()) / "shell output";
    auto config = [&](std::string const& marker)
    {
        return "[commands]\n"
            "literal = {argv = ['/bin/sh', '-c', 'out=$1; shift; printf \"<%s>\" \"$@\" > \"$out\"', "
            "'capture', '" + literal_path.string() + "', '', '$HOME; untouched', 'two words']}\n"
            "script = {shell = \"printf '%s' \\\"$(printf " + marker + ")\\\" > '" + shell_path.string() + "'\"}\n"
            "[[binds]]\nkey = 'F6'\nspawn = {ref = 'literal'}\n"
            "[[binds]]\nkey = 'F7'\nspawn = {ref = 'script'}\n";
    };
    REQUIRE(env->wm.write_config(config("first")));
    REQUIRE(send_ipc_command(*socket, "reload-config") == "ok reloaded");
    REQUIRE(send_key(env->conn, XK_F6));
    REQUIRE(wait_for_condition([&] { return read_text_file(literal_path) == "<><$HOME; untouched><two words>"; }, kTimeout));
    REQUIRE(send_key(env->conn, XK_F7));
    REQUIRE(wait_for_condition([&] { return read_text_file(shell_path) == "first"; }, kTimeout));

    REQUIRE(env->wm.write_config(config("second")));
    REQUIRE(send_ipc_command(*socket, "reload-config") == "ok reloaded");
    REQUIRE(send_key(env->conn, XK_F7));
    REQUIRE(wait_for_condition([&] { return read_text_file(shell_path) == "second"; }, kTimeout));

    REQUIRE(env->wm.write_config(config("rejected") + "[[binds]]\nkey = 'F8'\naction = 'state'\n"));
    auto rejected = send_ipc_command(*socket, "reload-config");
    REQUIRE(rejected);
    REQUIRE(rejected->starts_with("error "));
    REQUIRE(std::filesystem::remove(shell_path));
    REQUIRE(send_key(env->conn, XK_F7));
    REQUIRE(wait_for_condition([&] { return read_text_file(shell_path) == "second"; }, kTimeout));
}
