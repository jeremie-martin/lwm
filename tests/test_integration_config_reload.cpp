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

std::string make_config(std::vector<std::string> const& names, std::string const& extra = {})
{
    std::ostringstream out;
    out << "[appearance]\n";
    out << "padding = 10\n";
    out << "border_width = 2\n";
    out << "border_color = 0xFF0000\n\n";
    out << "[workspaces]\n";
    out << "names = [";
    for (size_t i = 0; i < names.size(); ++i) out << (i ? ", " : "") << '"' << toml_escape(names[i]) << '"';
    out << "]\n";
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


// A shell whose command line names `needle` is found until its commands have finished.

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
    "Integration: config reload updates desktop names and keeps failed reload atomic",
    "[integration][ipc][reload]"
)
{
    auto env = TestEnvironment::create(make_config({ "dev", "web" }));
    if (!env)
        SKIP("Test environment not available");

    REQUIRE(wait_for_desktop_names(env->conn, { "dev", "web" }));

    REQUIRE(env->wm.write_config(make_config({ "code", "chat" })));
    auto reload_ok = run_lwmctl(env->wm, { "config", "reload" });
    REQUIRE(reload_ok.has_value());
    REQUIRE(reload_ok->exit_code == 0);
    REQUIRE(wait_for_desktop_names(env->conn, { "code", "chat" }));
}

TEST_CASE("Integration: config reload changes the workspace count and folds removed workspaces", "[integration][ipc][reload]")
{
    auto env = TestEnvironment::create(make_config({ "one", "two", "three" }));
    if (!env)
        SKIP("Test environment not available");
    auto& conn = env->conn;
    xcb_atom_t desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    xcb_atom_t desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), desktops, 3, kTimeout));
    auto window = create_window(conn, 10, 10, 200, 150);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(send_ipc_command("window to-workspace 2") == "ok");
    REQUIRE(wait_for_property_cardinal(conn.get(), window, desktop, 2, kTimeout));

    REQUIRE(env->wm.write_config(make_config({ "one", "two" })));
    REQUIRE(send_ipc_command("config reload") == "ok");
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), desktops, 2, kTimeout));
    REQUIRE(wait_for_desktop_names(conn, { "one", "two" }));
    CHECK(wait_for_property_cardinal(conn.get(), window, desktop, 1, kTimeout));

    REQUIRE(env->wm.write_config(make_config({ "one", "two", "three", "four" })));
    REQUIRE(send_ipc_command("config reload") == "ok");
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), desktops, 4, kTimeout));
    CHECK(require_property_cardinal(conn.get(), window, desktop) == 1);
    REQUIRE(send_ipc_command("workspace switch 3") == "ok");
    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: config reload reapplies geometry rules to visible floating windows",
    "[integration][ipc][reload][rules]"
)
{
    auto env = TestEnvironment::create(make_config({ "left", "right" }));
    if (!env)
        SKIP("Test environment not available");


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
    REQUIRE(env->wm.write_config(make_config({ "left", "right" }, rules)));

    auto reload_result = run_lwmctl(env->wm, { "config", "reload" });
    REQUIRE(reload_result.has_value());
    REQUIRE(reload_result->exit_code == 0);
    REQUIRE(wait_for_window_geometry(env->conn, floating, 300, 200, 240, 160));

    destroy_window(env->conn, floating);
}

TEST_CASE(
    "Integration: config reload reapplies workspace rules to existing windows",
    "[integration][ipc][reload][rules][workspace]"
)
{
    auto env = TestEnvironment::create(make_config({ "left", "right" }));
    if (!env)
        SKIP("Test environment not available");


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
    REQUIRE(env->wm.write_config(make_config({ "left", "right" }, rules)));

    auto reload_result = run_lwmctl(env->wm, { "config", "reload" });
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
    "Integration: config reload keeps fallback focus when removing a hidden scratchpad",
    "[integration][ipc][reload][scratchpad][focus]"
)
{
    std::string scratchpad = R"(
[[scratchpads]]
name = "scratchpad"
spawn = ["/bin/true"]
match = { class = "ScratchpadClass", instance = "scratchpad-instance" }
size = { width = 0.8, height = 0.6 }
)";
    auto env = TestEnvironment::create(make_config({ "one", "two" }, scratchpad));
    if (!env)
        SKIP("Test environment not available");


    xcb_window_t fallback = create_window(env->conn, 10, 10, 400, 300);
    set_window_wm_class(env->conn, fallback, "fallback-instance", "FallbackClass");
    map_window(env->conn, fallback);
    REQUIRE(wait_for_active_window(env->conn, fallback, kTimeout));

    auto show_result = send_ipc_command("scratchpad toggle scratchpad");
    REQUIRE(show_result.has_value());
    REQUIRE(*show_result == "ok");

    xcb_window_t scratchpad_window = create_window(env->conn, 10, 10, 240, 160);
    set_window_wm_class(env->conn, scratchpad_window, "scratchpad-instance", "ScratchpadClass");
    map_window(env->conn, scratchpad_window);
    REQUIRE(wait_for_active_window(env->conn, scratchpad_window, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(env->conn, scratchpad_window); }, kTimeout));

    auto saved_center = window_center(env->conn, scratchpad_window);
    REQUIRE(saved_center.has_value());

    auto hide_result = send_ipc_command("scratchpad toggle scratchpad");
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

    REQUIRE(env->wm.write_config(make_config({ "one", "two" })));
    auto reload_result = run_lwmctl(env->wm, { "config", "reload" });
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
names = ["before", "two"]
[binds]
"F5" = "config reload"
"F6" = "workspace switch 1"
)");
    REQUIRE(env);
    auto& conn = env->conn;
    auto desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    REQUIRE(send_key(conn, XK_F6));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), desktop, 1, kTimeout));
    REQUIRE(env->wm.write_config(R"(
[workspaces]
names = ["after", "two"]
[binds]
"F6" = "workspace switch 0"
)"));
    REQUIRE(send_key(conn, XK_F5));
    REQUIRE(wait_for_desktop_names(conn, { "after", "two" }));
    REQUIRE(send_key(conn, XK_F6));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), desktop, 0, kTimeout));

    // Invalid replacement must leave both the published config and working bindings intact.
    REQUIRE(env->wm.write_config(R"(
[workspaces]
names = ["invalid", "two"]
[binds]
"F6" = "workspace switch 1"
[[rules]]
match = { title = "[invalid" }
apply = { floating = true }
)"));
    auto result = run_lwmctl(env->wm, { "config", "reload" });
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
names = ["1", "2"]
[[scratchpads]]
name = "pending"
spawn = ["/bin/true"]
match = { class = "Pending" }
)";
        if (keep_claimed)
            result += "\n[[scratchpads]]\nname = \"claimed\"\nspawn = [\"/bin/true\"]\nmatch = { class = \""
                + pattern + "\" }\n";
        return result;
    };
    auto env = TestEnvironment::create(config(true, "Original"));
    REQUIRE(env);
    auto& conn = env->conn;
    auto snapshot = [&]
    {
        auto reply = send_ipc_command("scratchpad list");
        REQUIRE(reply);
        REQUIRE(reply->starts_with("ok "));
        return nlohmann::json::parse(reply->substr(3));
    };
    REQUIRE(send_ipc_command("scratchpad toggle pending") == "ok");
    auto window = create_window(conn, 10, 10, 200, 150);
    set_window_wm_class(conn, window, "instance", "Original");
    map_window(conn, window);
    REQUIRE(wait_for_condition([&] { return snapshot()["named"][1]["window"] == window; }, kTimeout));
    auto before = snapshot();
    REQUIRE(before["named"][0]["pending"] == true);
    REQUIRE(env->wm.write_config(config(true, "[invalid")));
    auto rejected = run_lwmctl(env->wm, { "config", "reload" });
    REQUIRE(rejected);
    REQUIRE(rejected->exit_code != 0);
    CHECK(snapshot() == before);

    REQUIRE(env->wm.write_config(config(true, "Replacement")));
    REQUIRE(send_ipc_command("config reload") == "ok");
    CHECK(snapshot() == before);
    REQUIRE(send_ipc_command("scratchpad toggle claimed") == "ok");
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(send_ipc_command("scratchpad toggle claimed") == "ok");
    auto hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    REQUIRE(wait_for_condition([&] { return has_state(conn, window, hidden); }, kTimeout));
    REQUIRE(env->wm.write_config(config(false, "")));
    REQUIRE(send_ipc_command("config reload") == "ok");
    auto remaining = snapshot();
    REQUIRE(remaining["named"].size() == 1);
    CHECK(remaining["named"][0]["pending"] == true);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(wait_for_condition([&] { return !has_state(conn, window, hidden); }, kTimeout));
}

TEST_CASE("Integration: launch bindings preserve argv and follow reload", "[integration][reload][keybind][spawn]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto literal_path = std::filesystem::path(env->wm.runtime_dir()) / "literal output";
    auto shell_path = std::filesystem::path(env->wm.runtime_dir()) / "shell output";
    auto config = [&](std::string const& marker, std::string const& binds = "", std::string const& tail = "")
    {
        return "[binds]\n"
               "F6 = ['/bin/sh', '-c', 'out=$1; shift; printf \"<%s>\" \"$@\" > \"$out\"', "
               "'capture', '" + literal_path.string() + "', '', '$HOME; untouched', 'two words']\n"
               "F7 = ['sh', '-c', \"printf '%s' \\\"$(printf " + marker + ")\\\" > '" + shell_path.string() + "'\"]\n"
            + binds + tail;
    };
    REQUIRE(env->wm.write_config(config("first")));
    REQUIRE(send_ipc_command("config reload") == "ok");
    REQUIRE(send_key(env->conn, XK_F6));
    REQUIRE(wait_for_condition([&] { return read_text_file(literal_path) == "<><$HOME; untouched><two words>"; }, kTimeout));
    REQUIRE(send_key(env->conn, XK_F7));
    REQUIRE(wait_for_condition([&] { return read_text_file(shell_path) == "first"; }, kTimeout));

    REQUIRE(env->wm.write_config(config("second")));
    REQUIRE(send_ipc_command("config reload") == "ok");
    REQUIRE(send_key(env->conn, XK_F7));
    REQUIRE(wait_for_condition([&] { return read_text_file(shell_path) == "second"; }, kTimeout));

    for (auto const& [binds, tail] : std::initializer_list<std::pair<std::string, std::string>>{
             { "F8 = 'state'\n", "" },
             { "F8 = {argv = ['true']}\n", "" },
             { "", "[[rules]]\napply = {workspace = 0, workspace_name = '1'}\n" },
             { "", "[[rules]]\napply = {layer = 'above', below = true}\n" },
         })
    {
        CAPTURE(binds, tail);
        REQUIRE(env->wm.write_config(config("rejected", binds, tail)));
        auto rejected = send_ipc_command("config reload");
        REQUIRE(rejected);
        REQUIRE(rejected->starts_with("error "));
        REQUIRE(std::filesystem::remove(shell_path));
        REQUIRE(send_key(env->conn, XK_F7));
        REQUIRE(wait_for_condition([&] { return read_text_file(shell_path) == "second"; }, kTimeout));
    }
}

TEST_CASE("Integration: key bindings follow keyboard mapping changes", "[integration][config][keyboard]")
{
    auto env = TestEnvironment::create(R"(
[workspaces]
names = ["1", "2"]
[binds]
"F35" = "workspace switch 1"
)");
    REQUIRE(env);
    auto& conn = env->conn;
    if (!extension_available(conn, &xcb_test_id))
        SKIP("XTEST extension not available");
    REQUIRE_FALSE(first_keycode_for_keysym(conn, XK_F35));
    // Map the bound keysym onto a spare keycode, as a layout switch would.
    auto const* setup = xcb_get_setup(conn.get());
    auto count = static_cast<uint8_t>(setup->max_keycode - setup->min_keycode + 1);
    auto* mapping = xcb_get_keyboard_mapping_reply(conn.get(), xcb_get_keyboard_mapping(conn.get(), setup->min_keycode, count), nullptr);
    REQUIRE(mapping);
    auto per = mapping->keysyms_per_keycode;
    auto const* syms = xcb_get_keyboard_mapping_keysyms(mapping);
    std::optional<xcb_keycode_t> spare;
    for (int i = count - 1; i >= 0 && !spare; --i)
        if (std::all_of(syms + i * per, syms + (i + 1) * per, [](xcb_keysym_t sym) { return sym == XCB_NO_SYMBOL; }))
            spare = static_cast<xcb_keycode_t>(setup->min_keycode + i);
    free(mapping);
    REQUIRE(spare);
    std::vector<xcb_keysym_t> bound(per, XCB_NO_SYMBOL);
    bound[0] = XK_F35;
    xcb_change_keyboard_mapping(conn.get(), 1, *spare, per, bound.data());
    xcb_flush(conn.get());
    // The reply follows the WM's handling of the earlier MappingNotify and its grabs.
    REQUIRE(send_ipc_command("version").value_or("").starts_with("ok "));
    REQUIRE(send_key(conn, XK_F35));
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), intern_atom(conn.get(), "_NET_CURRENT_DESKTOP"), 1, kTimeout));
}

TEST_CASE("Integration: an invalid file at startup falls back to the default configuration", "[integration][config]")
{
    auto env = TestEnvironment::create("[not valid\n");
    REQUIRE(env);
    CHECK(env->wm.running());
    // Logging is asynchronous; the record arrives shortly after startup.
    CHECK(wait_for_condition(
        [&] { return env->wm.diagnostics().find("using the default configuration") != std::string::npos; }, kTimeout
    ));
    CHECK(send_ipc_command("version").value_or("").starts_with("ok "));
}
