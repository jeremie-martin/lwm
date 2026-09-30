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

CommandConfig make_shell_command(std::string value) { return CommandConfig::shell_command(std::move(value)); }

KeybindConfig make_spawn_bind(std::string mod, std::string key, std::string command)
{
    KeybindConfig keybind;
    keybind.mod = std::move(mod);
    keybind.key = std::move(key);
    keybind.action = SpawnAction{ make_shell_command(std::move(command)) };
    return keybind;
}

KeybindConfig make_action_bind(std::string mod, std::string key, Action action)
{
    return { std::move(mod), std::move(key), std::move(action) };
}

template <typename T> T const* action_as(Action const& action) { return std::get_if<T>(&action); }

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

std::unique_ptr<Connection> make_connection()
{
    std::unique_ptr<Connection> connection;
    std::string error;
    // The isolated server can briefly refuse connections during a reset.
    bool connected = lwm::test::wait_for_condition(
        [&]()
        {
            try
            {
                connection = std::make_unique<Connection>();
                return true;
            }
            catch (std::exception const& e)
            {
                error = e.what();
                return false;
            }
        },
        std::chrono::seconds(1)
    );
    INFO(error);
    REQUIRE(connected);
    return connection;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Modifier parsing tests (no X11 needed)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("KeybindManager parses modifier names, combinations and malformed input", "[keybind]")
{
    for (auto const& [text, expected] : std::vector<std::pair<std::string, uint16_t>>{
             {                "super",                                                                XCB_MOD_MASK_4 },
             {                "shift",                                                            XCB_MOD_MASK_SHIFT },
             {                 "ctrl",                                                          XCB_MOD_MASK_CONTROL },
             {              "control",                                                          XCB_MOD_MASK_CONTROL },
             {                  "alt",                                                                XCB_MOD_MASK_1 },
             {          "super+shift",                                         (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT) },
             {           "super+ctrl",                                       (XCB_MOD_MASK_4 | XCB_MOD_MASK_CONTROL) },
             {            "super+alt",                                             (XCB_MOD_MASK_4 | XCB_MOD_MASK_1) },
             {           "shift+ctrl",                                   (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL) },
             {             "ctrl+alt",                                       (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1) },
             {     "super+shift+ctrl",                  (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL) },
             {      "super+shift+alt",                        (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_1) },
             {       "super+ctrl+alt",                      (XCB_MOD_MASK_4 | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1) },
             {       "shift+ctrl+alt",                  (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1) },
             { "super+shift+ctrl+alt", (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1) },
             {              "unknown",                                                                             0 },
             {        "super+unknown",                                                                XCB_MOD_MASK_4 },
             {                     "",                                                                             0 },
             {     "shift+super+ctrl",                  (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL) },
             {     "ctrl+shift+super",                  (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL) },
             {               "super+",                                                                XCB_MOD_MASK_4 },
             {         "super+shift+",                                         (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT) },
             {                    "+",                                                                             0 },
             {                   "++",                                                                             0 },
             {                  "+++",                                                                             0 },
    })
    {
        CAPTURE(text);
        CHECK(KeybindManager::parse_modifier(text) == expected);
    }
}

TEST_CASE("KeybindManager preserves shell command payloads for spawn actions", "[keybind]")
{
    if (!ensure_x11_environment())
        SKIP("X11 environment not available");

    Config cfg = make_empty_config();
    cfg.keybinds.push_back(make_spawn_bind("super", "a", "/usr/bin/firefox"));
    auto conn = make_connection();
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
    auto conn = make_connection();
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
    cfg.keybinds.push_back(make_action_bind("super+shift", "a", KillAction{ }));
    cfg.keybinds.push_back(make_action_bind("super", "1", SwitchWorkspaceAction{ 0 }));
    cfg.keybinds.push_back(make_action_bind("super+shift", "1", MoveToWorkspaceAction{ 0 }));

    auto conn = make_connection();
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

    auto conn = make_connection();
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

    auto conn = make_connection();
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

    auto conn = make_connection();
    KeybindManager mgr(*conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;
    auto result = mgr.resolve(super, 0x1234);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("KeybindManager resolves configured actions and their payloads", "[keybind]")
{
    if (!ensure_x11_environment())
        SKIP("X11 environment not available");

    Config cfg = make_empty_config();
    cfg.keybinds.push_back(make_spawn_bind("super", "a", "terminal"));
    cfg.keybinds.push_back(make_action_bind("super", "q", KillAction{ }));
    cfg.keybinds.push_back(make_action_bind("super", "1", SwitchWorkspaceAction{ 0 }));
    cfg.keybinds.push_back(make_action_bind("super+shift", "1", MoveToWorkspaceAction{ 0 }));
    cfg.keybinds.push_back(make_action_bind("super", "Left", FocusMonitorAction{ -1 }));
    cfg.keybinds.push_back(make_action_bind("super+shift", "Left", MoveToMonitorAction{ -1 }));
    cfg.keybinds.push_back(make_action_bind("super", "f", ToggleFullscreenAction{ }));
    cfg.keybinds.push_back(make_action_bind("super", "j", FocusNextAction{ }));
    cfg.keybinds.push_back(make_action_bind("super", "k", FocusPrevAction{ }));

    auto conn = make_connection();
    KeybindManager mgr(*conn, cfg);

    uint16_t super = XCB_MOD_MASK_4;

    auto spawn = mgr.resolve(super, XStringToKeysym("a"));
    REQUIRE(spawn);
    REQUIRE(action_as<SpawnAction>(*spawn) != nullptr);
    auto resolved_q = mgr.resolve(super, XStringToKeysym("q"));
    REQUIRE(resolved_q);
    REQUIRE(action_as<KillAction>(*resolved_q));
    auto resolved_workspace = mgr.resolve(super, XStringToKeysym("1"));
    REQUIRE(resolved_workspace);
    REQUIRE(action_as<SwitchWorkspaceAction>(*resolved_workspace));
    auto left_monitor = mgr.resolve(super, XStringToKeysym("Left"));
    REQUIRE(left_monitor.has_value());
    REQUIRE(action_as<FocusMonitorAction>(*left_monitor) != nullptr);
    REQUIRE(action_as<FocusMonitorAction>(*left_monitor)->direction == -1);
    auto resolved_f = mgr.resolve(super, XStringToKeysym("f"));
    REQUIRE(resolved_f);
    REQUIRE(action_as<ToggleFullscreenAction>(*resolved_f));
    auto resolved_j = mgr.resolve(super, XStringToKeysym("j"));
    REQUIRE(resolved_j);
    REQUIRE(action_as<FocusNextAction>(*resolved_j));
    auto resolved_k = mgr.resolve(super, XStringToKeysym("k"));
    REQUIRE(resolved_k);
    REQUIRE(action_as<FocusPrevAction>(*resolved_k));
    auto move_workspace = mgr.resolve(super | XCB_MOD_MASK_SHIFT, XStringToKeysym("1"));
    REQUIRE(move_workspace);
    REQUIRE(action_as<MoveToWorkspaceAction>(*move_workspace));
    CHECK(action_as<MoveToWorkspaceAction>(*move_workspace)->workspace == 0);
    auto move_monitor = mgr.resolve(super | XCB_MOD_MASK_SHIFT, XStringToKeysym("Left"));
    REQUIRE(move_monitor);
    REQUIRE(action_as<MoveToMonitorAction>(*move_monitor));
    CHECK(action_as<MoveToMonitorAction>(*move_monitor)->direction == -1);
}
