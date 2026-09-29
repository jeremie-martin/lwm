#include "lwm/core/ipc_command.hpp"
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace lwm::ipc;
TEST_CASE("IPC command definitions drive CLI encoding and typed wire parsing", "[ipc][command]")
{
    for (auto const& spec : command_specs())
    {
        CAPTURE(spec.name);
        std::vector<std::string> args;
        std::string name(spec.name);
        size_t start = 0;
        while (start < name.size())
        {
            auto end = name.find(' ', start);
            args.push_back(name.substr(start, end - start));
            if (end == name.npos)
                break;
            start = end + 1;
        }
        switch (spec.argument)
        {
            case Argument::None:
                break;
            case Argument::Text:
                args.push_back(spec.id == CommandId::Layout ? "monocle" : "a name with spaces");
                break;
            case Argument::Number:
                args.push_back("+0.25");
                break;
            case Argument::Index:
                args.push_back("2");
                break;
            case Argument::Window:
                args.push_back("window=0x123");
                break;
            case Argument::Filter:
                args.push_back("focus_change");
                args.push_back("state_change");
                break;
        }
        auto encoded = encode_command(args);
        REQUIRE(encoded);
        auto decoded = parse_command(*encoded);
        REQUIRE(decoded);
        CHECK(decoded->id == spec.id);
    }
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
