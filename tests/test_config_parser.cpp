#include "lwm/config/config.hpp"
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
            std::unique_ptr<FILE, decltype(&fclose)> file(fdopen(fd.fd, "w"), &fclose);
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
    return load_config_result(file.path());
}

template <typename T> T const* action_as(Action const& action) { return std::get_if<T>(&action); }

} // namespace

TEST_CASE("Config parser loads commands, binds, workspace bind groups, and structured rules", "[config]")
{
    auto loaded = load_from_string(R"(
[commands]
terminal = { argv = ["/usr/bin/ghostty"] }
launcher = { shell = "rofi -show drun" }
notify = { argv = ["/usr/bin/printf", "hi"] }

[workspaces]
count = 3
names = ["code", "chat", "misc"]

[[scratchpads]]
name = "term"
spawn = { ref = "terminal" }
match = { class = "Ghostty", title = "dropdown" }
size = { width = 0.8, height = 0.6 }

[autostart]
commands = [{ ref = "notify" }]

[[binds]]
key = "super+Return"
spawn = { ref = "terminal" }

[[binds]]
key = "super+u"
toggle_scratchpad = "term"

[[workspace_binds]]
mode = "switch"
mod = "super"
keys = ["1", "2", "3"]

[[workspace_binds]]
mode = "move"
mod = "super+shift"
keys = ["F1", "F2", "F3"]

[[rules]]
match = { title = "dropdown" }
apply = { floating = true, scratchpad = "term", center = true }
)");

    REQUIRE(loaded.has_value());

    auto const& cfg = *loaded;
    REQUIRE(cfg.commands.contains("terminal"));
    REQUIRE(cfg.commands.at("terminal").kind == CommandConfig::Kind::Argv);
    REQUIRE(cfg.autostart.commands.size() == 1);
    REQUIRE(cfg.autostart.commands.front().argv.front() == "/usr/bin/printf");
    REQUIRE(cfg.scratchpads.size() == 1);
    REQUIRE(cfg.keybinds.size() == 8);
    REQUIRE(action_as<action::Spawn>(cfg.keybinds.at({ XCB_MOD_MASK_4, XK_Return })) != nullptr);
    auto const* scratchpad_action = action_as<action::ScratchpadToggle>(cfg.keybinds.at({ XCB_MOD_MASK_4, XK_u }));
    REQUIRE(scratchpad_action != nullptr);
    REQUIRE(scratchpad_action->name == "term");
    REQUIRE(cfg.rules.size() == 1);
    REQUIRE(cfg.rules[0].actions.scratchpad == "term");
}

TEST_CASE("Config parser recognizes swap_next and swap_prev actions", "[config][keybind]")
{
    auto loaded = load_from_string(R"(
[[binds]]
key = "super+shift+j"
swap_next = true

[[binds]]
key = "super+shift+k"
swap_prev = true
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
[binds]
key = "super+q"
kill = true
)");

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().find("binds") != std::string::npos);
    CHECK(loaded.error().find("array") != std::string::npos);
}

TEST_CASE("Config parser regenerates default bindings from overridden commands and workspace count", "[config]")
{
    auto loaded = load_from_string(R"(
[commands]
terminal = { argv = ["/usr/bin/ghostty"] }

[workspaces]
count = 3
)");

    REQUIRE(loaded.has_value());
    REQUIRE_FALSE(loaded->keybinds.empty());
    auto const* spawn_action = action_as<action::Spawn>(loaded->keybinds.at({ XCB_MOD_MASK_4, XK_Return }));
    REQUIRE(spawn_action != nullptr);
    REQUIRE(spawn_action->command.argv.front() == "/usr/bin/ghostty");

    size_t switch_bind_count = 0;
    for (auto const& keybind : loaded->keybinds)
    {
        if (action_as<action::SwitchWorkspace>(keybind.second))
            ++switch_bind_count;
    }
    REQUIRE(switch_bind_count == 6);
}

TEST_CASE("Config parser keeps non-workspace defaults when only workspace bind groups are overridden", "[config]")
{
    auto loaded = load_from_string(R"(
[commands]
terminal = { argv = ["/usr/bin/ghostty"] }

[workspaces]
count = 3

[[workspace_binds]]
mode = "switch"
mod = "super"
keys = ["F1", "F2", "F3"]
)");

    REQUIRE(loaded.has_value());

    bool saw_terminal_spawn = false;
    bool saw_focus_monitor = false;
    size_t switch_bind_count = 0;
    size_t move_bind_count = 0;

    for (auto const& keybind : loaded->keybinds)
    {
        if (auto const* spawn = action_as<action::Spawn>(keybind.second); spawn && keybind.first.keysym == XK_Return)
        {
            saw_terminal_spawn =
                spawn->command.kind == CommandConfig::Kind::Argv && spawn->command.argv.front() == "/usr/bin/ghostty";
        }
        if (action_as<action::FocusMonitor>(keybind.second))
            saw_focus_monitor = true;
        if (action_as<action::SwitchWorkspace>(keybind.second))
            ++switch_bind_count;
        if (action_as<action::MoveToWorkspace>(keybind.second))
            ++move_bind_count;
    }

    CHECK(saw_terminal_spawn);
    CHECK(saw_focus_monitor);
    CHECK(switch_bind_count == 3);
    CHECK(move_bind_count == 6);
}

TEST_CASE("Config parser rejects invalid key combos and duplicate bindings", "[config]")
{
    SECTION("invalid key")
    {
        auto loaded = load_from_string(R"(
[[binds]]
key = "super+DefinitelyNotAKeysym"
kill = true
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("unknown key 'DefinitelyNotAKeysym'") != std::string::npos);
    }

    SECTION("duplicate binding")
    {
        auto loaded = load_from_string(R"(
[workspaces]
count = 2

[[binds]]
key = "super+1"
switch_workspace = 0

[[workspace_binds]]
mode = "switch"
mod = "super"
keys = ["1", "2"]
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("duplicates an existing binding") != std::string::npos);
    }
}

TEST_CASE("Config parser rejects missing command refs and invalid regexes", "[config]")
{
    SECTION("missing command ref")
    {
        auto loaded = load_from_string(R"(
[[binds]]
key = "super+Return"
spawn = { ref = "missing_alias" }
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("unknown command 'missing_alias'") != std::string::npos);
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
count = 2
names = ["code", "chat"]

[[rules]]
match = { class = "Ghostty" }
apply = { workspace_name = "oops" }
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("unknown workspace 'oops'") != std::string::npos);
    }

    SECTION("conflicting workspace selectors")
    {
        auto loaded = load_from_string(R"(
[workspaces]
count = 2
names = ["code", "chat"]

[[rules]]
match = { class = "Ghostty" }
apply = { workspace = 0, workspace_name = "code" }
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("cannot define both 'workspace' and 'workspace_name'") != std::string::npos);
    }

    SECTION("conflicting monitor selectors")
    {
        auto loaded = load_from_string(R"(
[[rules]]
match = { class = "Ghostty" }
apply = { monitor = 0, monitor_name = "HDMI-1" }
)");

        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().find("cannot define both 'monitor' and 'monitor_name'") != std::string::npos);
    }
}

TEST_CASE("Config parser rejects rule geometry outside X11 ranges", "[config][rules]")
{
    for (auto field : { "x", "y", "width", "height" })
    {
        bool position = std::string_view(field) == "x" || std::string_view(field) == "y";
        for (auto value : { position ? "-32769" : "0", position ? "32768" : "65536", "4294967296" })
        {
            INFO(field << " = " << value);
            auto loaded =
                load_from_string(std::string("[[rules]]\napply.geometry = { ") + field + " = " + value + " }");
            REQUIRE_FALSE(loaded.has_value());
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
                              "[workspaces]\ncount = 4294967296",
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
count = 2
[[workspace_binds]]
mode = "move"
mod = "shift+super"
keys = ["F1", "F2"]
)");
    REQUIRE(loaded);
    size_t moves = 0;
    for (auto const& [binding, action] : loaded->keybinds)
        if (std::holds_alternative<action::MoveToWorkspace>(action))
            ++moves;
    CHECK(moves == 2); // Replaces the default super+shift group, regardless of ordering.
    auto const& action = loaded->keybinds.at({ XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, XK_F2 });
    REQUIRE(std::holds_alternative<action::MoveToWorkspace>(action));
    CHECK(std::get<action::MoveToWorkspace>(action).workspace == 1);

    for (std::string combo : { "super+", "super++a", "+a", "super+super+a", "ctrl+control+a", "unknown+a" })
    {
        CAPTURE(combo);
        CHECK_FALSE(load_from_string("[[binds]]\nkey = \"" + combo + "\"\nkill = true\n"));
    }
    for (std::string modifiers : { "super+", "+super", "super++shift", "ctrl+control" })
    {
        CAPTURE(modifiers);
        CHECK_FALSE(
            load_from_string("[[mousebinds]]\nmod = \"" + modifiers + "\"\nbutton = 1\naction = \"drag_window\"\n")
        );
    }
    auto duplicate = load_from_string(R"(
[[binds]]
key = "control+shift+a"
kill = true
[[binds]]
key = "shift+ctrl+a"
restart = true
)");
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().find("duplicates") != std::string::npos);
}

TEST_CASE("Parsed matchers preserve full matching and reject invalid rules and scratchpads", "[config][rules]")
{
    auto loaded = load_from_string(R"(
[[rules]]
match = { class = "Firefox|Chromium", instance = "Navigator", title = ".*Video.*", type = "DIALOG" }
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
            CHECK_FALSE(load_from_string("[[scratchpads]]\nname = \"term\"\nspawn = { argv = [\"true\"] }\n" + matcher)
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
spawn = { argv = ["true"] }
match = { class = "Term" }

[[binds]]
key = "super+m"
set_layout = "monocle"

[[binds]]
key = "super+r"
set_ratio = 0.4

[[binds]]
key = "super+l"
adjust_ratio = 0.05

[[binds]]
key = "super+e"
exec = "/usr/local/bin/lwm"

[[binds]]
key = "super+c"
cancel_scratchpad_launch = "term"

[[binds]]
key = "super+n"
next_workspace = true
)");
    REQUIRE(loaded);
    auto const& binds = loaded->keybinds;
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_m }) == Action{ action::SetLayout{ LayoutStrategy::Monocle } });
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_r }) == Action{ action::SetRatio{ 0.4 } });
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_l }) == Action{ action::AdjustRatio{ 0.05 } });
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_e }) == Action{ action::Exec{ "/usr/local/bin/lwm" } });
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_c }) == Action{ action::ScratchpadCancelLaunch{ "term" } });
    CHECK(binds.at({ XCB_MOD_MASK_4, XK_n }) == Action{ action::CycleWorkspace{ 1 } });

    for (auto const* text : { "[[binds]]\nkey = \"super+a\"\nset_layout = \"spiral\"\n",
                              "[layout]\nmin_ratio = 0.2\n[[binds]]\nkey = \"super+a\"\nset_ratio = 0.9\n",
                              "[[binds]]\nkey = \"super+a\"\nexec = \"\"\n",
                              "[[binds]]\nkey = \"super+a\"\ntoggle_scratchpad = \"missing\"\n",
                              "[[binds]]\nkey = \"super+a\"\nratio_grow = true\n" })
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
apply = { workspace_name = "chat", monitor_name = "HDMI-1", below = true, geometry = { x = 5 } }

[[rules]]
match = { class = "B" }
apply = { above = false }
)");
    REQUIRE(loaded);
    auto const& first = loaded->rules[0].actions;
    CHECK(first.workspace == 2);
    CHECK(first.monitor == std::variant<size_t, std::string>{ std::string("HDMI-1") });
    CHECK(first.layer == LayerHint::Below);
    CHECK(first.geometry == Geometry{ 5, 0, 800, 600 });
    // An explicit false clears a layer preference rather than leaving it unspecified.
    CHECK(loaded->rules[1].actions.layer == LayerHint::Normal);
    CHECK(loaded->layout.strategy == LayoutStrategy::MasterStack);
}

TEST_CASE("Configuration decoding preserves absence, explicit false, and empty bindings", "[config]")
{
    auto defaults = load_from_string("");
    REQUIRE(defaults);
    CHECK_FALSE(defaults->keybinds.empty());
    CHECK_FALSE(defaults->mousebinds.empty());
    CHECK(defaults->workspaces.names.size() == 10);

    auto empty = load_from_string("binds = []\nmousebinds = []");
    REQUIRE(empty);
    CHECK(empty->keybinds.empty());
    CHECK(empty->mousebinds.empty());

    auto rules = load_from_string(R"(
[[rules]]
match = { transient = false }
apply = { floating = false, fullscreen = false, sticky = false, skip_taskbar = false, skip_pager = false, borderless = false }
[[rules]]
apply = { center = true }
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
}

TEST_CASE("Configuration rejects malformed nested values at their owning field", "[config]")
{
    for (auto const& [text, field] : std::initializer_list<std::pair<std::string, std::string>>{
             {                              "[focus]\nwarp_cursor_on_monitor_change = 0", "warp_cursor_on_monitor_change" },
             {                                             "[appearance]\npadding = 1.5",                       "padding" },
             {                                         "[appearance]\nborder_color = -1",                  "border_color" },
             {                                 "[appearance]\nborder_color = 4294967296",                  "border_color" },
             {                    "[[mousebinds]]\nbutton = 256\naction = 'drag_window'",                        "button" },
             {                                  "[[mousebinds]]\naction = 'drag_window'",                        "button" },
             {                                                  "[[binds]]\nkill = true",                           "key" },
             {                                         "[[binds]]\nkey = 'F1'\nkill = 1",                          "kill" },
             {                                     "[[binds]]\nkey = 'F1'\nkill = false",                          "kill" },
             {                           "[[binds]]\nkey = 'F1'\nswitch_workspace = 1.0",              "switch_workspace" },
             {         "[[binds]]\nkey = 'F1'\nspawn = { argv = ['true'], typo = true }",                          "typo" },
             {                           "[commands]\nterminal = { argv = ['true', 1] }",                          "argv" },
             {                                               "[commands]\nterminal = {}",                      "terminal" },
             {              "[commands]\nterminal = { argv = ['true'], shell = 'true' }",                      "terminal" },
             {                              "[commands]\nterminal = { ref = 'browser' }",                           "ref" },
             {                                    "[commands]\nterminal = { argv = [] }",                          "argv" },
             {                                  "[commands]\nterminal = { argv = [''] }",                          "argv" },
             {                      "[[binds]]\nkey = 'F1'\nkill = true\nrestart = true",            "exactly one action" },
             { "[[scratchpads]]\nname = 'term'\nspawn = { argv = ['true'] }\nmatch = {}",                       "matcher" },
             {                                         "[unknown_section]\nvalue = true",               "unknown_section" },
             {                    "[[rules]]\napply.geometry = { width = 10, typo = 1 }",                          "typo" },
             { "[[rules]]\nmatch = { transient = 'false' }\napply = { floating = true }",                     "transient" },
             {                                      "[[rules]]\nmatch = { class = 'X' }",                         "apply" },
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
spawn = { argv = ['true'] }
match = { class = 'Term' }
size = { width = 1, height = 1 }
[[binds]]
key = 'F1'
adjust_ratio = 0
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
        CHECK_FALSE(load_from_string("[[binds]]\nkey = 'F1'\nadjust_ratio = " + value));
        CHECK_FALSE(load_from_string("[[binds]]\nkey = 'F1'\nset_ratio = " + value));
        CHECK_FALSE(load_from_string(
            "[[scratchpads]]\nname = 'term'\nspawn = { argv = ['true'] }\n"
            "match = { class = 'Term' }\nsize = { width = "
            + value + " }"
        ));
    }
}
