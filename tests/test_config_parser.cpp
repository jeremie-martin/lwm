#include "lwm/config/config.hpp"
#include "lwm/core/command.hpp"
#include "test_resources.hpp"
#include <X11/keysym.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <variant>
#include <vector>

using namespace lwm;

namespace {

class TempConfigFile
{
public:
    explicit TempConfigFile(std::string contents)
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "lwm-config-XXXXXX.toml").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');

        lwm::test::TestFd fd{ mkstemps(buffer.data(), 5) };
        REQUIRE(fd.fd >= 0);

        path_ = buffer.data();
        try
        {
            auto close = [](FILE* stream) { std::fclose(stream); };
            std::unique_ptr<FILE, decltype(close)> file(fdopen(fd.fd, "w"));
            REQUIRE(file);
            fd.fd = -1; // fdopen transfers ownership to FILE.
            REQUIRE(std::fwrite(contents.data(), 1, contents.size(), file.get()) == contents.size());
            REQUIRE(std::fclose(file.release()) == 0);
        }
        catch (...)
        {
            unlink(path_.c_str());
            throw;
        }
    }

    ~TempConfigFile()
    {
        if (!path_.empty())
        {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    std::string const& path() const { return path_; }

private:
    std::string path_;
};

ConfigLoadResult load_from_string(std::string const& contents)
{
    TempConfigFile file(contents);
    return load_config(file.path(), true);
}

template <typename T> T const* action_as(Action const& action) { return std::get_if<T>(&action); }

} // namespace

TEST_CASE("Config parser loads bindings, workspace keys, scratchpads, and structured rules", "[config]")
{
    auto loaded = load_from_string(R"(
[workspaces]
names = ["code", "chat", "misc"]

[[scratchpads]]
name = "term"
spawn = ["/usr/bin/ghostty"]
match = { class = "Ghostty", title = "dropdown" }
size = { width = 0.8, height = 0.6 }

[binds]
"super+Return" = ["/usr/bin/ghostty"]
"super+u" = "scratchpad toggle term"

[[workspace_keys]]
switch = "super"
keys = ["1", "2", "3"]
[[workspace_keys]]
move = "super+shift"
keys = ["F1", "F2", "F3"]
[[rules]]
match = { title = "dropdown" }
apply = { floating = true, geometry = { } }
)");

    REQUIRE(loaded.has_value());

    auto const& cfg = *loaded;
    REQUIRE(cfg.scratchpads.front().spawn == std::vector<std::string>{ "/usr/bin/ghostty" });
    REQUIRE(cfg.scratchpads.size() == 1);
    REQUIRE(cfg.keybinds.size() == 8);
    REQUIRE(action_as<action::Spawn>(cfg.keybinds.at({ XCB_MOD_MASK_4, XK_Return })) != nullptr);
    auto const* scratchpad_action = action_as<action::ScratchpadToggle>(cfg.keybinds.at({ XCB_MOD_MASK_4, XK_u }));
    REQUIRE(scratchpad_action != nullptr);
    REQUIRE(scratchpad_action->name == "term");
    REQUIRE(cfg.rules.size() == 1);
}

TEST_CASE("Config parser recognizes window swap next and prev", "[config][keybind]")
{
    auto loaded = load_from_string(R"(
[binds]
"super+shift+j" = "window swap next"
"super+shift+k" = "window swap prev"

)");

    REQUIRE(loaded.has_value());
    CHECK(loaded->keybinds.at({ XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, XK_j }) == Action{ action::SwapTile{ 1 } });
    CHECK(loaded->keybinds.at({ XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, XK_k }) == Action{ action::SwapTile{ -1 } });
}

TEST_CASE("Config parser rejects unknown layout strategies", "[config][layout]")
{
    auto loaded = load_from_string(R"(
[layout]
strategy = "not-a-strategy"
)");

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().find("[layout].strategy") != std::string::npos);
}

TEST_CASE("Config parser rejects unknown keys", "[config]")
{
    auto loaded = load_from_string(R"(
[appearance]
padding = 10
retired_overlay = true
)");

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().find("appearance") != std::string::npos);
    CHECK(loaded.error().find("retired_overlay") != std::string::npos);
}

TEST_CASE("Config parser rejects wrong top-level section types", "[config]")
{
    auto loaded = load_from_string(R"(
[[binds]]
key = "super+q"
action = "window close"
)");

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().find("binds") != std::string::npos);
}

TEST_CASE("A configuration file binds exactly what it lists", "[config]")
{
    auto loaded = load_from_string(R"(
[workspaces]
names = ["1", "2", "3"]
)");

    REQUIRE(loaded.has_value());
    CHECK(loaded->keybinds.empty());
    CHECK(loaded->mousebinds.empty());
    CHECK(loaded->workspaces == std::vector<std::string>{ "1", "2", "3" });
    CHECK(loaded->appearance.border_width == Config{ }.appearance.border_width);
}

TEST_CASE("The built-in defaults are the example file", "[config]")
{
    auto defaults = default_config();
    CHECK(defaults.mousebinds.size() == 3);
    CHECK(action_as<action::Spawn>(defaults.keybinds.at({ XCB_MOD_MASK_4, XK_Return })));
    CHECK(defaults.keybinds.at({ XCB_MOD_MASK_4, XK_1 }) == Action{ action::SwitchWorkspace{ 0 } });
    CHECK(defaults.keybinds.at({ XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, XK_ampersand }) == Action{ action::MoveToWorkspace{ 0 } });
}

TEST_CASE("Mouse bindings take grips or anything a key binding takes", "[config][mouse]")
{
    auto loaded = load_from_string(R"(
[workspaces]
names = ["a", "b"]
[mousebinds]
"super+1" = "move"
"super+3" = "resize"
"super+4" = "workspace switch 1"
"super+5" = "config reload"
"super+8" = ["xterm", "-e", "top"]
)");
    REQUIRE(loaded);
    auto bound = [&](uint8_t button) {
        auto it = std::ranges::find(loaded->mousebinds, button, &MousebindConfig::button);
        REQUIRE(it != loaded->mousebinds.end());
        CHECK(it->modifier == XCB_MOD_MASK_4);
        return it->action;
    };
    CHECK(bound(1) == std::variant<MouseGrip, Action>{ MouseGrip::Move });
    CHECK(bound(3) == std::variant<MouseGrip, Action>{ MouseGrip::Resize });
    CHECK(bound(4) == std::variant<MouseGrip, Action>{ Action{ action::SwitchWorkspace{ 1 } } });
    CHECK(bound(5) == std::variant<MouseGrip, Action>{ Action{ action::ReloadConfig{ } } });
    CHECK(bound(8) == std::variant<MouseGrip, Action>{ Action{ action::Spawn{ { "xterm", "-e", "top" } } } });
    // Mouse commands are validated like key bindings: queries and out-of-range values fail.
    for (std::string text : { "window list", "workspace switch 2", "drag" })
    {
        CAPTURE(text);
        CHECK_FALSE(load_from_string("[workspaces]\nnames = ['a', 'b']\n[mousebinds]\n'super+1' = '" + text + "'\n"));
    }
}

TEST_CASE("Workspace key groups switch, move, or both", "[config][keybind]")
{
    auto loaded = load_from_string(R"(
[workspaces]
names = ["a", "b"]

[[workspace_keys]]
switch = "super"
move = "super+shift"
keys = ["1", "2"]

[[workspace_keys]]
switch = "alt"
keys = ["F1", "F2"]
)");
    REQUIRE(loaded.has_value());
    CHECK(loaded->keybinds.size() == 6);
    CHECK(loaded->keybinds.at({ XCB_MOD_MASK_4, XK_2 }) == Action{ action::SwitchWorkspace{ 1 } });
    CHECK(loaded->keybinds.at({ XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, XK_2 }) == Action{ action::MoveToWorkspace{ 1 } });
    CHECK(loaded->keybinds.at({ XCB_MOD_MASK_1, XK_F1 }) == Action{ action::SwitchWorkspace{ 0 } });

    auto neither = load_from_string("[workspaces]\nnames = [\"a\"]\n[[workspace_keys]]\nkeys = [\"1\"]\n");
    REQUIRE_FALSE(neither);
    CHECK(neither.error().find("must define switch, move, or both") != std::string::npos);
    auto short_group = load_from_string("[[workspace_keys]]\nswitch = \"super\"\nkeys = [\"1\"]\n");
    REQUIRE_FALSE(short_group);
    CHECK(short_group.error().find("exactly 10 entries") != std::string::npos);
}

TEST_CASE("Config parser rejects invalid key combos and duplicate bindings", "[config]")
{
    SECTION("invalid key")
    {
        auto loaded = load_from_string(R"(
[binds]
"super+DefinitelyNotAKeysym" = "window close"
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("unknown key 'DefinitelyNotAKeysym'") != std::string::npos);
    }

    SECTION("duplicate binding")
    {
        auto loaded = load_from_string(R"(
[workspaces]
names = ["1", "2"]

[binds]
"super+1" = "workspace switch 0"

[[workspace_keys]]
switch = "super"
keys = ["1", "2"])");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("duplicates an existing binding") != std::string::npos);
    }
}

TEST_CASE("Config parser rejects unknown commands and invalid regexes", "[config]")
{
    SECTION("unknown command")
    {
        auto loaded = load_from_string(R"(
[binds]
"super+Return" = "launch terminal"
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("[binds].super+Return: unknown command") != std::string::npos);
    }

    SECTION("invalid rule regex")
    {
        auto loaded = load_from_string(R"(
[[rules]]
match = { title = "[broken(" }
apply = { floating = true }
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("invalid regex") != std::string::npos);
    }

    SECTION("empty apply table")
    {
        auto loaded = load_from_string(R"(
[[rules]]
match = { class = "Ghostty" }
apply = {}
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("must define at least one action") != std::string::npos);
    }

    SECTION("unknown workspace_name")
    {
        auto loaded = load_from_string(R"(
[workspaces]
names = ["code", "chat"]

[[rules]]
match = { class = "Ghostty" }
apply = { workspace = "oops" }
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("unknown workspace 'oops'") != std::string::npos);
    }

    SECTION("conflicting workspace selectors")
    {
        auto loaded = load_from_string(R"(
[workspaces]
names = ["code", "chat"]

[[rules]]
match = { class = "Ghostty" }
apply = { workspace = 0, workspace_name = "code" }
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("workspace_name") != std::string::npos);
    }

    SECTION("conflicting monitor selectors")
    {
        auto loaded = load_from_string(R"(
[[rules]]
match = { class = "Ghostty" }
apply = { monitor = 0, monitor_name = "HDMI-1" }
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("monitor_name") != std::string::npos);
    }
}

TEST_CASE("Config parser rejects rule geometry outside X11 ranges", "[config][rules]")
{
    // Positions come in pairs, so each out-of-range coordinate has a valid partner.
    for (auto field : { "x", "y", "width", "height" })
    {
        std::string_view name = field;
        bool position = name == "x" || name == "y";
        auto partner = name == "x" ? ", y = 0" : name == "y" ? ", x = 0" : "";
        for (auto value : { position ? "-32769" : "0", position ? "32768" : "65536", "4294967296" })
        {
            INFO(field << " = " << value);
            auto loaded = load_from_string(
                std::string("[[rules]]\napply.geometry = { ") + field + " = " + value + partner + " }"
            );
            REQUIRE_FALSE(loaded.has_value());
            CHECK(loaded.error().find(field) != std::string::npos);
        }
    }
    for (auto geometry :
         { "x = -32768, y = 32767, width = 1, height = 65535", "x = 32767, y = -32768, width = 65535, height = 1" })
    {
        auto loaded = load_from_string(std::string("[[rules]]\napply.geometry = { ") + geometry + " }");
        REQUIRE(loaded.has_value());
    }
}

TEST_CASE("Numeric configuration rejects non-finite values before range checks", "[config]")
{
    for (std::string value : { "nan", "+nan", "-nan", "inf", "+inf", "-inf" })
    {
        CAPTURE(value);
        CHECK_FALSE(load_from_string("[layout]\ndefault_ratio = " + value + "\n"));
        CHECK_FALSE(load_from_string("[layout]\nmin_ratio = " + value + "\n"));
    }
}

TEST_CASE("Config rejects numeric narrowing and excessive workspace allocation", "[config][bounds]")
{
    for (auto const* text : { "[appearance]\npadding = 4294967296",
                              "[appearance]\nborder_width = 65536",
                              "[workspaces]\nnames = []",
                              "[[rules]]\nmatch = { class = 'X' }\napply = { monitor = 4294967296 }" })
    {
        INFO(text);
        CHECK_FALSE(load_from_string(text));
    }
    CHECK(load_from_string("[appearance]\npadding = 65535\nborder_width = 65535"));
}

TEST_CASE("Binding identity uses modifiers and keysyms rather than spelling", "[config][keybind]")
{
    auto loaded = load_from_string(R"(
[workspaces]
names = ["1", "2"]
[[workspace_keys]]
move = "shift+super"
keys = ["F1", "F2"])");
    REQUIRE(loaded);
    size_t moves = 0;
    for (auto const& [binding, action] : loaded->keybinds)
        if (std::holds_alternative<action::MoveToWorkspace>(action))
            ++moves;
    CHECK(moves == 2); // Modifier order does not matter.
    auto const& action = loaded->keybinds.at({ XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, XK_F2 });
    REQUIRE(std::holds_alternative<action::MoveToWorkspace>(action));
    CHECK(std::get<action::MoveToWorkspace>(action).workspace == 1);

    for (std::string combo : { "super+", "super++a", "+a", "super+super+a", "ctrl+control+a", "unknown+a" })
    {
        CAPTURE(combo);
        CHECK_FALSE(load_from_string("[binds]\n\"" + combo + "\" = 'window close'\n"));
    }
    for (std::string modifiers : { "super+", "+super", "super++shift", "ctrl+control" })
    {
        CAPTURE(modifiers);
        CHECK_FALSE(
            load_from_string("[mousebinds]\n\"" + modifiers + "+1\" = \"move\"\n")
        );
    }
    auto duplicate = load_from_string(R"(
[binds]
"control+shift+a" = "window close"
"shift+ctrl+a" = "restart"
)");
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().find("duplicates") != std::string::npos);
}

TEST_CASE("Parsed matchers preserve full matching and reject invalid rules and scratchpads", "[config][rules]")
{
    auto loaded = load_from_string(R"(
[[rules]]
match = { class = "Firefox|Chromium", instance = "Navigator", title = ".*Video.*", type = "dialog" }
apply = { floating = true }
)");
    REQUIRE(loaded);
    auto const& rule = loaded->rules.front();
    CHECK(rule.type == lwm::WindowType::Dialog);
    CHECK(rule.match.matches("Firefox", "Navigator", "Video playing"));
    CHECK_FALSE(rule.match.matches("MyFirefox", "Navigator", "Video playing"));
    CHECK_FALSE(rule.match.matches("Firefox", "navigator", "Video playing"));
    CHECK_FALSE(rule.match.matches("Firefox", "Navigator", "Music playing"));
    for (std::string field : { "class", "instance", "title" })
        for (std::string pattern : { "", "[invalid" })
        {
            CAPTURE(field, pattern);
            auto matcher = "match = { " + field + " = \"" + pattern + "\" }\n";
            CHECK_FALSE(load_from_string("[[rules]]\n" + matcher + "apply = { floating = true }\n"));
            CHECK_FALSE(load_from_string("[[scratchpads]]\nname = \"term\"\nspawn = [\"true\"]\n" + matcher)
            );
        }
    CHECK_FALSE(load_from_string("[[rules]]\nmatch = { type = \"unknown\" }\napply = { floating = true }\n"));
}

TEST_CASE("Bindings accept every WM action with typed, validated values", "[config][keybind]")
{
    auto loaded = load_from_string(R"(
[layout]
min_ratio = 0.2

[[scratchpads]]
name = "term"
spawn = ["true"]
match = { class = "Term" }

[binds]
"super+m" = "layout set monocle"
"super+r" = "ratio set 0.4"
"super+l" = "ratio adjust 0.05"
"super+e" = "exec /usr/local/bin/lwm"
"super+c" = "scratchpad cancel-launch term"
"super+n" = "workspace next"

)");
    REQUIRE(loaded);
    auto const& binds = loaded->keybinds;
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_m }) == Action{ action::SetLayout{ LayoutStrategy::Monocle } });
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_r }) == Action{ action::SetRatio{ 0.4 } });
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_l }) == Action{ action::AdjustRatio{ 0.05 } });
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_e }) == Action{ action::Restart{ "/usr/local/bin/lwm" } });
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_c }) == Action{ action::ScratchpadCancelLaunch{ "term" } });
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_n }) == Action{ action::CycleWorkspace{ 1 } });

    for (auto const* text : { "[binds]\n\"super+a\" = 'layout set spiral'\n",
                              "[layout]\nmin_ratio = 0.2\n[binds]\n\"super+a\" = 'ratio set 0.9'\n",
                              "[binds]\n\"super+a\" = 'exec'\n",
                              "[binds]\n\"super+a\" = 'scratchpad toggle missing'\n",
                              "[binds]\n\"super+a\" = 'ratio grow'\n" })
    {
        CAPTURE(text);
        CHECK_FALSE(load_from_string(text));
    }
}

TEST_CASE("Rules resolve typed actions when the configuration loads", "[config][rules]")
{
    auto loaded = load_from_string(R"(
[workspaces]
names = ["web", "code", "chat"]

[[rules]]
match = { class = "A" }
apply = { workspace = "chat", monitor = "HDMI-1", layer = "below", geometry = { x = 5, y = -6, height = 7 } }

[[rules]]
match = { class = "B" }
apply = { layer = "normal" }
)");
    REQUIRE(loaded);
    auto const& first = loaded->rules[0].actions;
    CHECK(first.workspace == 2);
    CHECK(first.monitor == std::variant<size_t, std::string>{ std::string("HDMI-1") });
    CHECK(first.layer == LayerHint::Below);
    CHECK(first.geometry == RuleGeometry{ .position = std::pair<int16_t, int16_t>{ 5, -6 }, .height = 7 });
    // Selecting normal explicitly clears a layer preference.
    CHECK(loaded->rules[1].actions.layer == LayerHint::Normal);
    CHECK(loaded->layout.strategy == LayoutStrategy::MasterStack);
}

TEST_CASE("Configuration decoding preserves absence, explicit false, and empty sections", "[config]")
{
    auto empty_file = load_from_string("");
    REQUIRE(empty_file);
    CHECK(empty_file->keybinds.empty());
    CHECK(empty_file->mousebinds.empty());
    CHECK(empty_file->workspaces.size() == 10);

    auto empty = load_from_string("[binds]\n[mousebinds]\n");
    REQUIRE(empty);
    CHECK(empty->keybinds.empty());
    CHECK(empty->mousebinds.empty());

    auto rules = load_from_string(R"(
[[rules]]
match = { transient = false }
apply = { floating = false, fullscreen = false, sticky = false, skip_taskbar = false, skip_pager = false, borderless = false }
[[rules]]
apply = { geometry = { } }
)");
    REQUIRE(rules);
    REQUIRE(rules->rules.size() == 2);
    auto const& explicit_false = rules->rules[0];
    CHECK(explicit_false.transient == false);
    CHECK(explicit_false.actions.floating == false);
    CHECK(explicit_false.actions.fullscreen == false);
    CHECK(explicit_false.actions.sticky == false);
    CHECK(explicit_false.actions.skip_taskbar == false);
    CHECK(explicit_false.actions.skip_pager == false);
    CHECK(explicit_false.actions.borderless == false);
    CHECK_FALSE(rules->rules[1].transient.has_value());
    CHECK_FALSE(rules->rules[1].actions.floating.has_value());
    CHECK(rules->rules[1].actions.geometry == RuleGeometry{ });
    auto half = load_from_string("[[rules]]\napply = { geometry = { x = 5 } }\n");
    REQUIRE_FALSE(half);
    CHECK(half.error().find("must set both x and y") != std::string::npos);
}
TEST_CASE("Configuration rejects malformed nested values at their owning field", "[config]")
{
    for (auto const& [text, field] : std::initializer_list<std::pair<std::string, std::string>>{
             { "[focus]\nwarp_cursor_on_monitor_change = 0", "warp_cursor_on_monitor_change" },
             { "[appearance]\npadding = 1.5", "padding" },
             { "[appearance]\nborder_color = -1", "border_color" },
             { "[appearance]\nborder_color = 4294967296", "border_color" },
             { "[mousebinds]\n'super+256' = 'move'", "button number" },
             { "[mousebinds]\n'super' = 'move'", "button number" },
             { "[mousebinds]\n'super+1' = 'drag'", "mousebinds" },
             { "[mousebinds]\n'super+1' = 'move'\n'super+01' = 'window float'", "duplicates" },
             { "[binds]\n'F1' = 1", "binds" },
             { "[binds]\n'F1' = false", "binds" },
             { "[binds]\n'F1' = 'workspace switch 1.0'", "workspace index" },
             { "[binds]\n'F1' = []", "nonempty executable" },
             { "[binds]\n'F1' = ['']", "nonempty executable" },
             { "[[scratchpads]]\nname = 'term'\nspawn = []\nmatch = { class = 'T' }", "nonempty executable" },
             { "[[scratchpads]]\nname = 'term'\nspawn = ['true']\nmatch = {}", "matcher" },
             { "[workspaces]\ncount = 3", "count" },
             { "[commands]\nterminal = ['st']", "commands" },
             { "[autostart]\ncommands = []", "autostart" },
             { "[[workspace_binds]]\nmode = 'switch'", "workspace_binds" },
             { "[unknown_section]\nvalue = true", "unknown_section" },
             { "[[rules]]\napply.geometry = { width = 10, typo = 1 }", "typo" },
             { "[[rules]]\nmatch = { transient = 'false' }\napply = { floating = true }", "transient" },
             { "[[rules]]\nmatch = { class = 'X' }", "apply" },
         })
    {
        CAPTURE(text);
        auto loaded = load_from_string(text);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().find(field) != std::string::npos);
    }
}
TEST_CASE("Numeric action and scratchpad values accept integers without coercing other types", "[config]")
{
    auto loaded = load_from_string(R"(
[appearance]
border_color = 4294967295
[[scratchpads]]
name = 'term'
spawn = ['true']
match = { class = 'Term' }
size = { width = 1, height = 1 }
[binds]
'F1' = "ratio adjust 0"
)");
    REQUIRE(loaded);
    CHECK(loaded->appearance.border_color == UINT32_MAX);
    REQUIRE(loaded->scratchpads.size() == 1);
    CHECK(loaded->scratchpads[0].width == 1.0);
    CHECK(loaded->scratchpads[0].height == 1.0);
    CHECK(loaded->keybinds.at({ 0, XK_F1 }) == Action{ action::AdjustRatio{ 0.0 } });
    for (std::string value : { "nan", "inf", "-inf", "true", "'0.5'" })
    {
        CAPTURE(value);
        CHECK_FALSE(load_from_string("[binds]\n'F1' = \"ratio adjust " + value + "\""));
        CHECK_FALSE(load_from_string("[binds]\n'F1' = \"ratio set " + value + "\""));
        CHECK_FALSE(load_from_string(
            "[[scratchpads]]\nname = 'term'\nspawn = ['true']\n"
            "match = { class = 'Term' }\nsize = { width = "
            + value + " }"
        ));
    }
}

TEST_CASE("Bindings use the shared command grammar and reject non-actions", "[config][command][keybind]")
{
    for (std::string text : { "window fullscreen", "workspace switch 2", "monitor focus prev",
                             "window focus 0x123", "window attention 0x123", "ratio adjust +0.25",
                             "exec /tmp/a wm", "scratchpad toggle a name with spaces" })
    {
        CAPTURE(text);
        auto loaded = load_from_string(
            "[[scratchpads]]\nname = 'a name with spaces'\nspawn = ['true']\nmatch = {class = 'Term'}\n"
            "[binds]\n'F1' = '" + text + "'\n");
        REQUIRE(loaded);
        auto parsed = command::parse_command(text);
        REQUIRE(parsed);
        CHECK(loaded->keybinds.at({ 0, XK_F1 }) == std::get<Action>(*parsed));
    }
    for (std::string text : { "state", "window list", "version", "watch",
                             "workspace switch 10", "window to-workspace 10", "monitor focus up",
                             "scratchpad toggle missing", "scratchpad cancel-launch missing", "ratio set 0.99" })
    {
        CAPTURE(text);
        CHECK_FALSE(load_from_string("[binds]\n'F1' = '" + text + "'\n"));
    }
    CHECK_FALSE(load_from_string("[binds]\n'F1' = {}\n"));
}

TEST_CASE("Launch commands are argv lists executed without a shell", "[config][spawn]")
{
    auto loaded = load_from_string(R"(
[[scratchpads]]
name = 'term'
spawn = ['sh', '-c', 'printf "%s" "$HOME"']
match = {class = 'Term'}
[binds]
'F1' = ['/bin/printf', '%s', '', '$HOME; two words']
)");
    REQUIRE(loaded);
    std::vector<std::string> literal{ "/bin/printf", "%s", "", "$HOME; two words" };
    CHECK(std::get<action::Spawn>(loaded->keybinds.at({ 0, XK_F1 })).argv == literal);
    CHECK(loaded->scratchpads.front().spawn == std::vector<std::string>{ "sh", "-c", "printf \"%s\" \"$HOME\"" });
    auto rejected = load_from_string(R"([binds]
'F1' = ["true", "a\u0000b"]
)");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("NUL") != std::string::npos);
}
TEST_CASE("Configuration choices lower to one runtime meaning", "[config][rules]")
{
    for (std::string selector : { "1", "'code'" })
    {
        CAPTURE(selector);
        auto config = load_from_string(
            "[workspaces]\nnames = [\"web\", \"code\"]\n[[rules]]\napply = {workspace = " + selector + "}\n"
        );
        REQUIRE(config);
        CHECK(config->rules.front().actions.workspace == 1);
        CHECK_FALSE(config->rules.front().actions.monitor);
        CHECK_FALSE(config->rules.front().actions.layer);
    }
    for (auto const& [name, layer] : {
             std::pair{ "normal", LayerHint::Normal },
             std::pair{ "above", LayerHint::Above },
             std::pair{ "below", LayerHint::Below }
         })
    {
        CAPTURE(name);
        auto config = load_from_string("[[rules]]\napply = {layer = '" + std::string(name) + "'}\n");
        REQUIRE(config);
        CHECK(config->rules.front().actions.layer == layer);
    }
    auto monitor = load_from_string("[[rules]]\napply = {monitor = 0}\n[[rules]]\napply = {monitor = 'DUMMY1'}\n");
    REQUIRE(monitor);
    CHECK(monitor->rules[0].actions.monitor == std::variant<size_t, std::string>{ size_t{ 0 } });
    CHECK(monitor->rules[1].actions.monitor == std::variant<size_t, std::string>{ std::string("DUMMY1") });

    auto maximum = load_from_string("[[rules]]\napply = {monitor = 2147483647}\n");
    REQUIRE(maximum);
    CHECK(maximum->rules.front().actions.monitor == std::variant<size_t, std::string>{ size_t{ 2147483647 } });
    for (std::string field : { "workspace", "monitor" })
        for (std::string invalid : { "true", "1.0", "-1", "65535", "2147483648", "[]", "{}" })
        {
            // Monitor indices have a wider range than workspace indices.
            if (field == "monitor" && invalid == "65535")
                continue;
            CAPTURE(field, invalid);
            CHECK_FALSE(load_from_string("[[rules]]\napply = {" + field + " = " + invalid + "}\n"));
        }
    for (std::string apply : { "workspace_name = 'web'", "monitor_name = 'DUMMY1'", "above = true", "below = false",
                              "layer = true", "layer = 'Above'", "layer = 'invalid'" })
    {
        CAPTURE(apply);
        CHECK_FALSE(load_from_string("[[rules]]\napply = {" + apply + "}\n"));
    }
}

TEST_CASE("Launch commands must be nonempty argv lists at every use site", "[config][spawn]")
{
    for (std::string command : { "{}", "[]", "['']", "['true', 1]", "{argv = ['true']}", "{shell = 'true'}" })
    {
        CAPTURE(command);
        CHECK_FALSE(load_from_string("[binds]\n'F1' = " + command + "\n"));
        CHECK_FALSE(load_from_string("[[scratchpads]]\nname = 'term'\nmatch = {class = 'Term'}\nspawn = " + command + "\n"));
    }
    CHECK(load_from_string("[binds]\n'F1' = ['true']\n"));
}
TEST_CASE("Rule window types name the types that become clients", "[config][rules]")
{
    for (auto const& [name, type] : {
             std::pair{ "normal", WindowType::Normal }, std::pair{ "dialog", WindowType::Dialog },
             std::pair{ "utility", WindowType::Utility }, std::pair{ "toolbar", WindowType::Toolbar },
             std::pair{ "menu", WindowType::Menu }, std::pair{ "splash", WindowType::Splash }
         })
    {
        CAPTURE(name);
        auto config = load_from_string(
            "[[rules]]\nmatch = {type = '" + std::string(name) + "'}\napply = {floating = true}\n"
        );
        REQUIRE(config);
        CHECK(config->rules.front().type == type);
    }
    for (std::string invalid : { "'Dialog'", "'DIALOG'", "'dock'", "'desktop'", "'popup_menu'", "0", "true" })
    {
        CAPTURE(invalid);
        CHECK_FALSE(load_from_string("[[rules]]\nmatch = {type = " + invalid + "}\napply = {floating = true}\n"));
    }
}
