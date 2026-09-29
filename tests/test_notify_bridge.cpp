#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
using namespace lwm::test;

TEST_CASE("Notification bridge forwards only exact window hints", "[ipc][notify][bridge]")
{
    if (!std::filesystem::exists("/usr/bin/jq"))
        SKIP("jq is required for the optional notification bridge");
    auto directory = std::filesystem::path(make_temp_dir());
    REQUIRE_FALSE(directory.empty());
    struct Cleanup
    {
        std::filesystem::path directory;
        ~Cleanup() { std::filesystem::remove_all(directory); }
    } cleanup{ directory };
    REQUIRE(write_text_file(directory / "busctl", R"(#!/bin/sh
cat <<'DATA'
{"payload":{"data":["app",0,"","","",[],{"x-window-id":{"data":123}},0]}}
{"payload":{"data":["app",0,"","","",[],{"x-window-id":{"data":"0x2a"}},0]}}
{"payload":{"data":["app",0,"","","",[],{"x-window-id":{"data":0}},0]}}
{"payload":{"data":["app",0,"","","",[],{"desktop-entry":{"data":"app"}},0]}}
{"payload":{"data":["app",0,"","","",[],{"x-window-id":{"data":"123; bad"}},0]}}
DATA
)"));
    REQUIRE(write_text_file(directory / "lwmctl", "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$BRIDGE_TRACE\"\n"));
    REQUIRE(chmod((directory / "busctl").c_str(), 0700) == 0);
    REQUIRE(chmod((directory / "lwmctl").c_str(), 0700) == 0);
    auto result = run_command(
        "/bin/bash",
        {
            LWM_NOTIFY_BRIDGE_PATH
    },
        { { "PATH", directory.string() + ":/usr/bin:/bin" }, { "BRIDGE_TRACE", (directory / "trace").string() } }
    );
    REQUIRE(result);
    INFO(result->stderr_text);
    CHECK(result->exit_code == 0);
    CHECK(read_text_file(directory / "trace") == "notify-attention window=123\nnotify-attention window=0x2a\n");
}
