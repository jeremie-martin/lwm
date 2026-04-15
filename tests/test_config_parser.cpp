#include "lwm/config/config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>
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

        int fd = mkstemps(buffer.data(), 5);
        REQUIRE(fd >= 0);

        path_ = buffer.data();
        FILE* file = fdopen(fd, "w");
        REQUIRE(file != nullptr);
        REQUIRE(std::fwrite(contents.data(), 1, contents.size(), file) == contents.size());
        REQUIRE(std::fclose(file) == 0);
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
    REQUIRE(cfg.keybinds[0].action == "spawn");
    REQUIRE(cfg.keybinds[0].command.has_value());
    REQUIRE(cfg.keybinds[1].action == "toggle_scratchpad");
    REQUIRE(cfg.keybinds[1].target == "term");
    REQUIRE(cfg.rules.size() == 1);
    REQUIRE(cfg.rules[0].scratchpad == "term");
}

TEST_CASE("Config parser rejects unknown keys", "[config]")
{
    auto loaded = load_from_string(R"(
[appearance]
padding = 10
enable_internal_bar = true
)");

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().find("unknown key 'enable_internal_bar'") != std::string::npos);
}

TEST_CASE("Config parser rejects wrong top-level section types", "[config]")
{
    auto loaded = load_from_string(R"(
[binds]
key = "super+q"
kill = true
)");

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().find("[[binds]] must be an array") != std::string::npos);
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
    REQUIRE(loaded->keybinds.front().action == "spawn");
    REQUIRE(loaded->keybinds.front().command.has_value());
    REQUIRE(loaded->keybinds.front().command->argv.front() == "/usr/bin/ghostty");

    size_t switch_bind_count = 0;
    for (auto const& keybind : loaded->keybinds)
    {
        if (keybind.action == "switch_workspace")
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
        if (keybind.action == "spawn" && keybind.key == "Return" && keybind.command.has_value())
        {
            saw_terminal_spawn = keybind.command->kind == CommandConfig::Kind::Argv
                && keybind.command->argv.front() == "/usr/bin/ghostty";
        }
        if (keybind.action == "focus_monitor")
            saw_focus_monitor = true;
        if (keybind.action == "switch_workspace")
            ++switch_bind_count;
        if (keybind.action == "move_to_workspace")
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
