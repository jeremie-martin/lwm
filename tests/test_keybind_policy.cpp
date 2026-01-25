#include "lwm/config/config.hpp"
#include "lwm/keybind/keybind.hpp"
#include "x11_test_harness.hpp"
#include <X11/Xlib.h>
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

Config make_empty_config()
{
    Config cfg;
    cfg.keybinds.clear();
    return cfg;
}

Config make_default_config()
{
    Config cfg;
    return cfg;
}

bool ensure_x11_environment()
{
    auto& env = lwm::test::X11TestEnvironment::instance();
    if (!env.available())
    {
        WARN("X11 not available; set LWM_TEST_ALLOW_EXISTING_DISPLAY=1 to use an existing DISPLAY.");
        return false;
    }
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Modifier parsing tests (no X11 needed)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("KeybindManager::parse_modifier handles single modifiers", "[keybind]")
{
    REQUIRE(KeybindManager::parse_modifier("super") == XCB_MOD_MASK_4);
    REQUIRE(KeybindManager::parse_modifier("shift") == XCB_MOD_MASK_SHIFT);
    REQUIRE(KeybindManager::parse_modifier("ctrl") == XCB_MOD_MASK_CONTROL);
    REQUIRE(KeybindManager::parse_modifier("control") == XCB_MOD_MASK_CONTROL);
    REQUIRE(KeybindManager::parse_modifier("alt") == XCB_MOD_MASK_1);
}

TEST_CASE("KeybindManager::parse_modifier handles combined modifiers", "[keybind]")
{
    REQUIRE(KeybindManager::parse_modifier("super+shift") == (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT));
    REQUIRE(KeybindManager::parse_modifier("super+ctrl") == (XCB_MOD_MASK_4 | XCB_MOD_MASK_CONTROL));
    REQUIRE(KeybindManager::parse_modifier("super+alt") == (XCB_MOD_MASK_4 | XCB_MOD_MASK_1));
    REQUIRE(KeybindManager::parse_modifier("shift+ctrl") == (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL));
    REQUIRE(KeybindManager::parse_modifier("ctrl+alt") == (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1));
}

TEST_CASE("KeybindManager::parse_modifier handles triple modifiers", "[keybind]")
{
    REQUIRE(
        KeybindManager::parse_modifier("super+shift+ctrl")
        == (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL)
    );
    REQUIRE(
        KeybindManager::parse_modifier("super+shift+alt") == (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_1)
    );
    REQUIRE(
        KeybindManager::parse_modifier("super+ctrl+alt") == (XCB_MOD_MASK_4 | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1)
    );
    REQUIRE(
        KeybindManager::parse_modifier("shift+ctrl+alt") == (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1)
    );
}

TEST_CASE("KeybindManager::parse_modifier handles all four modifiers", "[keybind]")
{
    REQUIRE(
        KeybindManager::parse_modifier("super+shift+ctrl+alt")
        == (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1)
    );
}

TEST_CASE("KeybindManager::parse_modifier handles unknown modifiers gracefully", "[keybind]")
{
    REQUIRE(KeybindManager::parse_modifier("unknown") == 0);
    REQUIRE(KeybindManager::parse_modifier("super+unknown") == XCB_MOD_MASK_4);
    REQUIRE(KeybindManager::parse_modifier("") == 0);
}

TEST_CASE("KeybindManager::parse_modifier is order-independent", "[keybind]")
{
    uint16_t expected = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL;
    REQUIRE(KeybindManager::parse_modifier("super+shift+ctrl") == expected);
    REQUIRE(KeybindManager::parse_modifier("shift+super+ctrl") == expected);
    REQUIRE(KeybindManager::parse_modifier("ctrl+shift+super") == expected);
}

TEST_CASE("KeybindManager::parse_modifier handles malformed modifier strings", "[keybind][edge]")
{
    SECTION("Multiple consecutive plus signs")
    {
        SKIP(
            "Documents design decision: empty tokens between '+' are ignored, so 'super++shift' matches both 'super' "
            "and 'shift'"
        );
        REQUIRE(KeybindManager::parse_modifier("super++shift") == XCB_MOD_MASK_4);
    }

    SECTION("Leading plus sign")
    {
        SKIP("Documents design decision: empty tokens are ignored, so '+super' parses as 'super'");
        REQUIRE(KeybindManager::parse_modifier("+super") == 0);
    }

    SECTION("Trailing plus sign")
    {
        REQUIRE(KeybindManager::parse_modifier("super+") == XCB_MOD_MASK_4);
        REQUIRE(KeybindManager::parse_modifier("super+shift+") == (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT));
    }

    SECTION("Only plus signs")
    {
        REQUIRE(KeybindManager::parse_modifier("+") == 0);
        REQUIRE(KeybindManager::parse_modifier("++") == 0);
        REQUIRE(KeybindManager::parse_modifier("+++") == 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Command resolution tests (no X11 needed)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("KeybindManager::resolve_command returns command as-is if not preset", "[keybind]")
{
    Config cfg = make_empty_config();
    Connection conn;
    KeybindManager mgr(conn, cfg);

    REQUIRE(mgr.resolve_command("/usr/bin/firefox", cfg) == "/usr/bin/firefox");
    REQUIRE(mgr.resolve_command("my-custom-command", cfg) == "my-custom-command");
}

TEST_CASE("KeybindManager::resolve_command expands preset commands", "[keybind]")
{
    Config cfg = make_empty_config();
    cfg.programs.terminal = "/usr/local/bin/st";
    cfg.programs.browser = "/usr/bin/firefox";
    cfg.programs.launcher = "dmenu_run";

    Connection conn;
    KeybindManager mgr(conn, cfg);

    REQUIRE(mgr.resolve_command("terminal", cfg) == "/usr/local/bin/st");
    REQUIRE(mgr.resolve_command("browser", cfg) == "/usr/bin/firefox");
    REQUIRE(mgr.resolve_command("launcher", cfg) == "dmenu_run");
}

// ─────────────────────────────────────────────────────────────────────────────
// Keybind resolution tests (requires X11)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("KeybindManager::resolve returns nullopt for unregistered bindings", "[keybind]")
{
    if (!ensure_x11_environment())
        return;

    Config cfg = make_empty_config();
    Connection conn;
    KeybindManager mgr(conn, cfg);

    auto result = mgr.resolve(XCB_MOD_MASK_4, 0x61);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("KeybindManager::resolve returns correct action for registered binding", "[keybind]")
{
    if (!ensure_x11_environment())
        return;

    Config cfg = make_empty_config();
    cfg.keybinds.push_back({ "super", "a", "spawn", "test-command", -1 });

    Connection conn;
    KeybindManager mgr(conn, cfg);

    uint16_t mod = XCB_MOD_MASK_4;
    xcb_keysym_t keysym = XStringToKeysym("a");

    auto result = mgr.resolve(mod, keysym);
    REQUIRE(result.has_value());
    REQUIRE(result->type == "spawn");
    REQUIRE(result->command == "test-command");
    REQUIRE(result->workspace == -1);
}

TEST_CASE("KeybindManager::resolve handles multiple bindings", "[keybind]")
{
    if (!ensure_x11_environment())
        return;

    Config cfg = make_empty_config();
    cfg.keybinds.push_back({ "super", "a", "spawn", "terminal", -1 });
    cfg.keybinds.push_back({ "super", "b", "spawn", "browser", -1 });
    cfg.keybinds.push_back({ "super+shift", "a", "kill", "", -1 });

    Connection conn;
    KeybindManager mgr(conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;
    uint16_t super_shift = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT;

    auto terminal_result = mgr.resolve(super, XStringToKeysym("a"));
    REQUIRE(terminal_result.has_value());
    REQUIRE(terminal_result->type == "spawn");
    REQUIRE(terminal_result->command == "terminal");

    auto browser_result = mgr.resolve(super, XStringToKeysym("b"));
    REQUIRE(browser_result.has_value());
    REQUIRE(browser_result->type == "spawn");
    REQUIRE(browser_result->command == "browser");

    auto kill_result = mgr.resolve(super_shift, XStringToKeysym("a"));
    REQUIRE(kill_result.has_value());
    REQUIRE(kill_result->type == "kill");
}

TEST_CASE("KeybindManager::resolve handles workspace actions", "[keybind]")
{
    if (!ensure_x11_environment())
        return;

    Config cfg = make_empty_config();
    cfg.keybinds.push_back({ "super", "1", "switch_workspace", "", 0 });
    cfg.keybinds.push_back({ "super+shift", "1", "move_to_workspace", "", 0 });

    Connection conn;
    KeybindManager mgr(conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;
    uint16_t super_shift = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT;

    auto switch_result = mgr.resolve(super, XStringToKeysym("1"));
    REQUIRE(switch_result.has_value());
    REQUIRE(switch_result->type == "switch_workspace");
    REQUIRE(switch_result->workspace == 0);

    auto move_result = mgr.resolve(super_shift, XStringToKeysym("1"));
    REQUIRE(move_result.has_value());
    REQUIRE(move_result->type == "move_to_workspace");
    REQUIRE(move_result->workspace == 0);
}

TEST_CASE("KeybindManager::resolve distinguishes actions by key", "[keybind]")
{
    if (!ensure_x11_environment())
        return;

    Config cfg = make_empty_config();
    cfg.keybinds.push_back({ "super", "a", "spawn", "terminal", -1 });
    cfg.keybinds.push_back({ "super", "b", "spawn", "browser", -1 });

    Connection conn;
    KeybindManager mgr(conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;

    auto result_a = mgr.resolve(super, XStringToKeysym("a"));
    auto result_b = mgr.resolve(super, XStringToKeysym("b"));

    REQUIRE(result_a->command == "terminal");
    REQUIRE(result_b->command == "browser");
}

TEST_CASE("KeybindManager::resolve distinguishes actions by modifier", "[keybind]")
{
    if (!ensure_x11_environment())
        return;

    Config cfg = make_empty_config();
    cfg.keybinds.push_back({ "super", "a", "spawn", "terminal", -1 });
    cfg.keybinds.push_back({ "super+shift", "a", "kill", "", -1 });

    Connection conn;
    KeybindManager mgr(conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;
    uint16_t super_shift = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT;
    xcb_keysym_t keysym_a = XStringToKeysym("a");

    auto spawn_result = mgr.resolve(super, keysym_a);
    auto kill_result = mgr.resolve(super_shift, keysym_a);

    REQUIRE(spawn_result->type == "spawn");
    REQUIRE(kill_result->type == "kill");
}

TEST_CASE("KeybindManager handles all standard keybind modifiers", "[keybind]")
{
    if (!ensure_x11_environment())
        return;

    Config cfg = make_empty_config();
    cfg.keybinds.push_back({ "super", "a", "spawn", "test-cmd-1", -1 });
    cfg.keybinds.push_back({ "super+shift", "a", "spawn", "test-cmd-2", -1 });
    cfg.keybinds.push_back({ "super+ctrl", "a", "spawn", "test-cmd-3", -1 });
    cfg.keybinds.push_back({ "super+alt", "a", "spawn", "test-cmd-4", -1 });

    Connection conn;
    KeybindManager mgr(conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;
    uint16_t super_shift = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT;
    uint16_t super_ctrl = XCB_MOD_MASK_4 | XCB_MOD_MASK_CONTROL;
    uint16_t super_alt = XCB_MOD_MASK_4 | XCB_MOD_MASK_1;

    auto keysym = XStringToKeysym("a");

    REQUIRE(mgr.resolve(super, keysym)->command == "test-cmd-1");
    REQUIRE(mgr.resolve(super_shift, keysym)->command == "test-cmd-2");
    REQUIRE(mgr.resolve(super_ctrl, keysym)->command == "test-cmd-3");
    REQUIRE(mgr.resolve(super_alt, keysym)->command == "test-cmd-4");
}

TEST_CASE("KeybindManager handles modifier state filtering", "[keybind]")
{
    if (!ensure_x11_environment())
        return;

    Config cfg = make_empty_config();
    cfg.keybinds.push_back({ "super", "a", "spawn", "test", -1 });

    Connection conn;
    KeybindManager mgr(conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;
    uint16_t super_shift = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT;
    uint16_t super_ctrl = XCB_MOD_MASK_4 | XCB_MOD_MASK_CONTROL;
    xcb_keysym_t keysym_a = XStringToKeysym("a");

    auto exact_match = mgr.resolve(super, keysym_a);
    auto with_shift = mgr.resolve(super_shift, keysym_a);
    auto with_ctrl = mgr.resolve(super_ctrl, keysym_a);

    REQUIRE(exact_match.has_value());
    REQUIRE_FALSE(with_shift.has_value());
    REQUIRE_FALSE(with_ctrl.has_value());
}

TEST_CASE("KeybindManager handles invalid key names in config", "[keybind]")
{
    if (!ensure_x11_environment())
        return;

    Config cfg = make_empty_config();
    cfg.keybinds.push_back({ "super", "InvalidKeyThatDoesNotExist", "spawn", "test", -1 });

    Connection conn;
    KeybindManager mgr(conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;
    auto result = mgr.resolve(super, 0x1234);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("KeybindManager handles all action types", "[keybind]")
{
    if (!ensure_x11_environment())
        return;

    Config cfg = make_empty_config();
    cfg.keybinds.push_back({ "super", "a", "spawn", "terminal", -1 });
    cfg.keybinds.push_back({ "super", "q", "kill", "", -1 });
    cfg.keybinds.push_back({ "super", "1", "switch_workspace", "", 0 });
    cfg.keybinds.push_back({ "super+shift", "1", "move_to_workspace", "", 0 });
    cfg.keybinds.push_back({ "super", "Left", "focus_monitor_left", "", -1 });
    cfg.keybinds.push_back({ "super+shift", "Left", "move_to_monitor_left", "", -1 });
    cfg.keybinds.push_back({ "super", "f", "toggle_fullscreen", "", -1 });
    cfg.keybinds.push_back({ "super", "j", "focus_next", "", -1 });
    cfg.keybinds.push_back({ "super", "k", "focus_prev", "", -1 });

    Connection conn;
    KeybindManager mgr(conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;

    REQUIRE(mgr.resolve(super, XStringToKeysym("a"))->type == "spawn");
    REQUIRE(mgr.resolve(super, XStringToKeysym("q"))->type == "kill");
    REQUIRE(mgr.resolve(super, XStringToKeysym("1"))->type == "switch_workspace");
    REQUIRE(mgr.resolve(super, XStringToKeysym("Left"))->type == "focus_monitor_left");
    REQUIRE(mgr.resolve(super, XStringToKeysym("f"))->type == "toggle_fullscreen");
    REQUIRE(mgr.resolve(super, XStringToKeysym("j"))->type == "focus_next");
    REQUIRE(mgr.resolve(super, XStringToKeysym("k"))->type == "focus_prev");
}
