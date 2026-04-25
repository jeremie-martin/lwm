#include "lwm/config/config.hpp"
#include "lwm/keybind/keybind.hpp"
#include "x11_test_harness.hpp"
#include <X11/Xlib.h>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <variant>

using namespace lwm;

namespace {

Config make_empty_config()
{
    Config cfg;
    cfg.keybinds.clear();
    return cfg;
}

CommandConfig make_shell_command(std::string value)
{
    return CommandConfig::shell_command(std::move(value));
}

KeybindConfig make_spawn_bind(std::string mod, std::string key, std::string command)
{
    KeybindConfig keybind;
    keybind.mod = std::move(mod);
    keybind.key = std::move(key);
    keybind.action = SpawnAction { make_shell_command(std::move(command)) };
    return keybind;
}

KeybindConfig make_action_bind(std::string mod, std::string key, std::string action, int workspace = -1, int direction = 0)
{
    KeybindConfig keybind;
    keybind.mod = std::move(mod);
    keybind.key = std::move(key);
    if (action == "kill")
        keybind.action = KillAction {};
    else if (action == "switch_workspace")
        keybind.action = SwitchWorkspaceAction { static_cast<size_t>(workspace) };
    else if (action == "move_to_workspace")
        keybind.action = MoveToWorkspaceAction { static_cast<size_t>(workspace) };
    else if (action == "focus_monitor")
        keybind.action = FocusMonitorAction { direction };
    else if (action == "move_to_monitor")
        keybind.action = MoveToMonitorAction { direction };
    else if (action == "toggle_fullscreen")
        keybind.action = ToggleFullscreenAction {};
    else if (action == "focus_next")
        keybind.action = FocusNextAction {};
    else if (action == "focus_prev")
        keybind.action = FocusPrevAction {};
    return keybind;
}

template<typename T>
T const* action_as(Action const& action)
{
    return std::get_if<T>(&action);
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

std::unique_ptr<Connection> make_connection_or_null()
{
    try
    {
        return std::make_unique<Connection>();
    }
    catch (std::exception const& e)
    {
        WARN(e.what());
        return nullptr;
    }
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
    // Trailing plus sign - ignored
    REQUIRE(KeybindManager::parse_modifier("super+") == XCB_MOD_MASK_4);
    REQUIRE(KeybindManager::parse_modifier("super+shift+") == (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT));

    // Only plus signs - returns 0 (no valid modifiers)
    REQUIRE(KeybindManager::parse_modifier("+") == 0);
    REQUIRE(KeybindManager::parse_modifier("++") == 0);
    REQUIRE(KeybindManager::parse_modifier("+++") == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Command payload tests (requires X11)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("KeybindManager preserves shell command payloads for spawn actions", "[keybind]")
{
    if (!ensure_x11_environment())
        SKIP("X11 environment not available");

    Config cfg = make_empty_config();
    cfg.keybinds.push_back(make_spawn_bind("super", "a", "/usr/bin/firefox"));
    auto conn = make_connection_or_null();
    if (!conn)
        SKIP("X11 connection not available");
    KeybindManager mgr(*conn, cfg);

    auto action = mgr.resolve(XCB_MOD_MASK_4, XStringToKeysym("a"));
    REQUIRE(action.has_value());
    auto const* spawn = action_as<SpawnAction>(*action);
    REQUIRE(spawn != nullptr);
    REQUIRE(spawn->command.kind == CommandConfig::Kind::Shell);
    REQUIRE(spawn->command.shell == "/usr/bin/firefox");
}

// ─────────────────────────────────────────────────────────────────────────────
// Keybind resolution tests (requires X11)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("KeybindManager::resolve returns nullopt for unregistered bindings", "[keybind]")
{
    if (!ensure_x11_environment())
        SKIP("X11 environment not available");

    Config cfg = make_empty_config();
    auto conn = make_connection_or_null();
    if (!conn)
        SKIP("X11 connection not available");
    KeybindManager mgr(*conn, cfg);

    auto result = mgr.resolve(XCB_MOD_MASK_4, 0x61);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("KeybindManager::resolve handles multiple bindings across keys, modifiers, and actions", "[keybind]")
{
    if (!ensure_x11_environment())
        SKIP("X11 environment not available");

    Config cfg = make_empty_config();
    cfg.keybinds.push_back(make_spawn_bind("super", "a", "terminal"));
    cfg.keybinds.push_back(make_spawn_bind("super", "b", "browser"));
    cfg.keybinds.push_back(make_action_bind("super+shift", "a", "kill"));
    cfg.keybinds.push_back(make_action_bind("super", "1", "switch_workspace", 0));
    cfg.keybinds.push_back(make_action_bind("super+shift", "1", "move_to_workspace", 0));

    auto conn = make_connection_or_null();
    if (!conn)
        SKIP("X11 connection not available");
    KeybindManager mgr(*conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;
    uint16_t super_shift = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT;

    // Distinguish by key
    auto result_a = mgr.resolve(super, XStringToKeysym("a"));
    auto result_b = mgr.resolve(super, XStringToKeysym("b"));
    REQUIRE(action_as<SpawnAction>(*result_a)->command.shell == "terminal");
    REQUIRE(action_as<SpawnAction>(*result_b)->command.shell == "browser");

    // Distinguish by modifier
    auto spawn_result = mgr.resolve(super, XStringToKeysym("a"));
    auto kill_result = mgr.resolve(super_shift, XStringToKeysym("a"));
    REQUIRE(action_as<SpawnAction>(*spawn_result) != nullptr);
    REQUIRE(action_as<KillAction>(*kill_result) != nullptr);

    // Workspace actions
    auto switch_result = mgr.resolve(super, XStringToKeysym("1"));
    auto move_result = mgr.resolve(super_shift, XStringToKeysym("1"));
    REQUIRE(action_as<SwitchWorkspaceAction>(*switch_result) != nullptr);
    REQUIRE(action_as<SwitchWorkspaceAction>(*switch_result)->workspace == 0);
    REQUIRE(action_as<MoveToWorkspaceAction>(*move_result) != nullptr);
    REQUIRE(action_as<MoveToWorkspaceAction>(*move_result)->workspace == 0);
}

TEST_CASE("KeybindManager handles all standard keybind modifiers", "[keybind]")
{
    if (!ensure_x11_environment())
        SKIP("X11 environment not available");

    Config cfg = make_empty_config();
    cfg.keybinds.push_back(make_spawn_bind("super", "a", "test-cmd-1"));
    cfg.keybinds.push_back(make_spawn_bind("super+shift", "a", "test-cmd-2"));
    cfg.keybinds.push_back(make_spawn_bind("super+ctrl", "a", "test-cmd-3"));
    cfg.keybinds.push_back(make_spawn_bind("super+alt", "a", "test-cmd-4"));

    auto conn = make_connection_or_null();
    if (!conn)
        SKIP("X11 connection not available");
    KeybindManager mgr(*conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;
    uint16_t super_shift = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT;
    uint16_t super_ctrl = XCB_MOD_MASK_4 | XCB_MOD_MASK_CONTROL;
    uint16_t super_alt = XCB_MOD_MASK_4 | XCB_MOD_MASK_1;

    auto keysym = XStringToKeysym("a");

    REQUIRE(action_as<SpawnAction>(*mgr.resolve(super, keysym))->command.shell == "test-cmd-1");
    REQUIRE(action_as<SpawnAction>(*mgr.resolve(super_shift, keysym))->command.shell == "test-cmd-2");
    REQUIRE(action_as<SpawnAction>(*mgr.resolve(super_ctrl, keysym))->command.shell == "test-cmd-3");
    REQUIRE(action_as<SpawnAction>(*mgr.resolve(super_alt, keysym))->command.shell == "test-cmd-4");
}

TEST_CASE("KeybindManager handles modifier state filtering", "[keybind]")
{
    if (!ensure_x11_environment())
        SKIP("X11 environment not available");

    Config cfg = make_empty_config();
    cfg.keybinds.push_back(make_spawn_bind("super", "a", "test"));

    auto conn = make_connection_or_null();
    if (!conn)
        SKIP("X11 connection not available");
    KeybindManager mgr(*conn, cfg);

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
        SKIP("X11 environment not available");

    Config cfg = make_empty_config();
    cfg.keybinds.push_back(make_spawn_bind("super", "InvalidKeyThatDoesNotExist", "test"));

    auto conn = make_connection_or_null();
    if (!conn)
        SKIP("X11 connection not available");
    KeybindManager mgr(*conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;
    auto result = mgr.resolve(super, 0x1234);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("KeybindManager handles all action types", "[keybind]")
{
    if (!ensure_x11_environment())
        SKIP("X11 environment not available");

    Config cfg = make_empty_config();
    cfg.keybinds.push_back(make_spawn_bind("super", "a", "terminal"));
    cfg.keybinds.push_back(make_action_bind("super", "q", "kill"));
    cfg.keybinds.push_back(make_action_bind("super", "1", "switch_workspace", 0));
    cfg.keybinds.push_back(make_action_bind("super+shift", "1", "move_to_workspace", 0));
    cfg.keybinds.push_back(make_action_bind("super", "Left", "focus_monitor", -1, -1));
    cfg.keybinds.push_back(make_action_bind("super+shift", "Left", "move_to_monitor", -1, -1));
    cfg.keybinds.push_back(make_action_bind("super", "f", "toggle_fullscreen"));
    cfg.keybinds.push_back(make_action_bind("super", "j", "focus_next"));
    cfg.keybinds.push_back(make_action_bind("super", "k", "focus_prev"));

    auto conn = make_connection_or_null();
    if (!conn)
        SKIP("X11 connection not available");
    KeybindManager mgr(*conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;

    REQUIRE(action_as<SpawnAction>(*mgr.resolve(super, XStringToKeysym("a"))) != nullptr);
    REQUIRE(action_as<KillAction>(*mgr.resolve(super, XStringToKeysym("q"))) != nullptr);
    REQUIRE(action_as<SwitchWorkspaceAction>(*mgr.resolve(super, XStringToKeysym("1"))) != nullptr);
    auto left_monitor = mgr.resolve(super, XStringToKeysym("Left"));
    REQUIRE(left_monitor.has_value());
    REQUIRE(action_as<FocusMonitorAction>(*left_monitor) != nullptr);
    REQUIRE(action_as<FocusMonitorAction>(*left_monitor)->direction == -1);
    REQUIRE(action_as<ToggleFullscreenAction>(*mgr.resolve(super, XStringToKeysym("f"))) != nullptr);
    REQUIRE(action_as<FocusNextAction>(*mgr.resolve(super, XStringToKeysym("j"))) != nullptr);
    REQUIRE(action_as<FocusPrevAction>(*mgr.resolve(super, XStringToKeysym("k"))) != nullptr);
}
