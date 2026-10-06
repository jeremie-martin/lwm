#include "lwm/core/command.hpp"
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace lwm;
using namespace lwm::command;

TEST_CASE("IPC grammar preserves public command spellings and typed requests", "[ipc][command]")
{
    // Literal public inputs are independent of the production command inventory.
    auto check = [](std::vector<std::string> const& argv, std::string const& wire, Request const& expected)
    {
        CAPTURE(wire);
        REQUIRE(encode_command(argv) == wire);
        auto decoded = parse_command(wire);
        REQUIRE(decoded);
        CHECK(*decoded == expected);
    };
    check({ "ping" }, "ping", Query::Ping);
    check({ "state" }, "state", Query::State);
    check({ "exec", "/tmp/a wm" }, "exec /tmp/a wm", Action{ action::Restart{ "/tmp/a wm" } });
    check({ "layout", "set", "monocle" }, "layout set monocle", Action{ action::SetLayout{ LayoutStrategy::Monocle } });
    check(
        { "scratchpad", "toggle", "a name with spaces" },
        "scratchpad toggle a name with spaces",
        Action{ action::ScratchpadToggle{ "a name with spaces" } }
    );
    check({ "ratio", "adjust", "+0.25" }, "ratio adjust +0.25", Action{ action::AdjustRatio{ 0.25 } });
    check({ "workspace", "switch", "2" }, "workspace switch 2", Action{ action::SwitchWorkspace{ 2 } });
    check({ "focus", "window=0x123" }, "focus window=0x123", Action{ action::FocusWindow{ 291 } });
}

TEST_CASE("IPC exposes every key-binding action except process launch", "[ipc][command]")
{
    auto request = [](std::string_view text)
    {
        auto parsed = parse_command(text);
        REQUIRE(parsed);
        return *parsed;
    };
    CHECK(request("window close") == Request{ Action{ action::Kill{ } } });
    CHECK(request("window fullscreen") == Request{ Action{ action::ToggleFullscreen{ } } });
    CHECK(request("window float") == Request{ Action{ action::ToggleFloat{ } } });
    CHECK(request("window swap next") == Request{ Action{ action::SwapTile{ 1 } } });
    CHECK(request("window swap prev") == Request{ Action{ action::SwapTile{ -1 } } });
    CHECK(request("window to-workspace 3") == Request{ Action{ action::MoveToWorkspace{ 3 } } });
    CHECK(request("window to-monitor left") == Request{ Action{ action::MoveToMonitor{ -1 } } });
    CHECK(request("monitor focus right") == Request{ Action{ action::FocusMonitor{ 1 } } });
    CHECK(request("workspace toggle") == Request{ Action{ action::ToggleWorkspace{ } } });
    CHECK(request("workspace next") == Request{ Action{ action::CycleWorkspace{ 1 } } });
    CHECK(request("ratio reset") == Request{ Action{ action::ResetRatios{ } } });
    CHECK(request("reload-config") == Request{ Action{ action::ReloadConfig{ } } });
}

TEST_CASE("IPC grammar rejects invalid arguments before execution", "[ipc][command]")
{
    for (std::string_view command : { "ping extra",
                                      "focus window=-1",
                                      "focus window=4294967296",
                                      "focus window=0x",
                                      "ratio set nan",
                                      "ratio adjust inf",
                                      "ratio set +",
                                      "workspace switch -1",
                                      "workspace switch 1junk",
                                      "layout set other",
                                      "monitor focus up",
                                      "window to-monitor",
                                      "scratchpad toggle",
                                      "exec /bin/lwm\nping" })
    {
        CAPTURE(command);
        CHECK_FALSE(parse_command(command));
    }
    CHECK_FALSE(encode_command(std::vector<std::string>{ "exec", "two", "arguments" }));
    CHECK_FALSE(encode_command(std::vector<std::string>{ "scratchpad", "toggle", "bad\nname" }));
    CHECK(parse_command("  ping  "));
    CHECK(*parse_command("focus window=0X123") == Request{ Action{ action::FocusWindow{ 0x123 } } });
}

TEST_CASE("Command diagnostics and CLI arity preserve the public grammar", "[ipc][command]")
{
    for (auto const& [input, message] : std::vector<std::pair<std::string, std::string>>{
             { "ping extra", "usage: ping" },
             { "exec", "usage: exec PATH" },
             { "layout set", "usage: layout set NAME" },
             { "layout set other", "unknown layout: other" },
             { "monitor focus up", "usage: monitor focus left|right" },
             { "ratio set +", "usage: ratio set VALUE" },
             { "ratio set nan", "invalid ratio value: nan" },
             { "ratio adjust inf", "invalid delta value: inf" },
             { "workspace switch 4294967296", "invalid workspace index: 4294967296" },
             { "workspace switch 0x10", "invalid workspace index: 0x10" },
             { "focus 10", "usage: focus window=<xid>" },
             { "focus window=0x", "invalid window id: 0x" },
             { "focus window=1 2", "usage: focus window=<xid>" },
         })
    {
        CAPTURE(input);
        auto parsed = parse_command(input);
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error() == message);
    }
    // Text is one CLI argument even when its wire value contains spaces.
    CHECK(encode_command(std::vector<std::string>{ "exec", "a path with spaces" }) == "exec a path with spaces");
    auto invalid_argv = [](std::vector<std::string> const& argv, std::string const& message)
    {
        auto encoded = encode_command(argv);
        REQUIRE_FALSE(encoded);
        CHECK(encoded.error() == message);
    };
    invalid_argv({ "exec", "a", "path" }, "usage: exec PATH");
    invalid_argv({ "ping", "" }, "usage: ping");
    invalid_argv({ "workspace", "switch", "2", "" }, "usage: workspace switch N");
    CHECK(*parse_command("workspace switch 4294967295")
          == Request{ Action{ action::SwitchWorkspace{ 4294967295U } } });
    CHECK(*parse_command("focus window=0xffffffff") == Request{ Action{ action::FocusWindow{ 4294967295U } } });
}
