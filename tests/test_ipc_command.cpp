#include "lwm/core/events.hpp"
#include "lwm/core/ipc_command.hpp"
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace lwm::ipc;
TEST_CASE("IPC grammar preserves public command spellings and typed arguments", "[ipc][command]")
{
    // Literal public inputs are independent of the production command inventory.
    auto check = [](std::vector<std::string> const& argv, std::string const& wire, Command const& expected)
    {
        CAPTURE(wire);
        REQUIRE(encode_command(argv) == wire);
        auto decoded = parse_command(wire);
        REQUIRE(decoded);
        CHECK(decoded->id == expected.id);
        CHECK(decoded->argument == expected.argument);
    };
    check({ "ping" }, "ping", { CommandId::Ping, { } });
    check({ "exec", "/tmp/a wm" }, "exec /tmp/a wm", { CommandId::Exec, std::string("/tmp/a wm") });
    check({ "layout", "set", "monocle" }, "layout set monocle", { CommandId::Layout, std::string("monocle") });
    check(
        { "scratchpad", "toggle", "a name with spaces" },
        "scratchpad toggle a name with spaces",
        { CommandId::Toggle, std::string("a name with spaces") }
    );
    check({ "ratio", "adjust", "+0.25" }, "ratio adjust +0.25", { CommandId::RatioAdjust, 0.25 });
    check({ "workspace", "switch", "2" }, "workspace switch 2", { CommandId::WorkspaceSwitch, uint32_t{ 2 } });
    check({ "focus", "window=0x123" }, "focus window=0x123", { CommandId::FocusWindow, uint32_t{ 291 } });
    check(
        { "subscribe", "focus_change", "state_change" },
        "subscribe focus_change,state_change",
        { CommandId::Subscribe, uint32_t{ lwm::Event_FocusChange | lwm::Event_StateChange } }
    );
    check({ "subscribe" }, "subscribe", { CommandId::Subscribe, uint32_t{ lwm::Event_All } });
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
                                      "scratchpad toggle",
                                      "subscribe unknown",
                                      "exec /bin/lwm\nping" })
    {
        CAPTURE(command);
        CHECK_FALSE(parse_command(command));
    }
    CHECK_FALSE(encode_command(std::vector<std::string>{ "exec", "two", "arguments" }));
    CHECK_FALSE(encode_command(std::vector<std::string>{ "scratchpad", "toggle", "bad\nname" }));
    CHECK(parse_command("  ping  "));
    CHECK(std::get<uint32_t>(parse_command("focus window=0X123")->argument) == 0x123);
}
