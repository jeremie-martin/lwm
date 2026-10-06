#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <mutex>
#include <thread>

using namespace lwm::test;

namespace {
// Owns WM_S0 on a display without a WM and answers each command with a scripted
// reply, exercising the real CLI's protocol independently of LWM. A foreign WM
// publishes no state.
struct FakeWm
{
    enum class Answer
    {
        Reply,
        Silence,
        Exit,
        Foreign
    };

    X11Connection conn;
    xcb_window_t window = XCB_NONE;
    std::mutex mutex;
    std::vector<std::string> requests;
    std::jthread thread;

    explicit FakeWm(std::string reply, Answer answer = Answer::Reply)
    {
        REQUIRE(conn.ok());
        REQUIRE(wm_owner(conn) == XCB_NONE);
        window = xcb_generate_id(conn.get());
        xcb_create_window(conn.get(), 0, window, conn.root(), -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
        xcb_set_selection_owner(conn.get(), window, intern_atom(conn.get(), "WM_S0"), XCB_CURRENT_TIME);
        auto command = intern_atom(conn.get(), "_LWM_COMMAND");
        auto response = intern_atom(conn.get(), "_LWM_REPLY");
        auto utf8 = intern_atom(conn.get(), "UTF8_STRING");
        REQUIRE(wm_owner(conn) == window);
        if (answer != Answer::Foreign)
            xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, intern_atom(conn.get(), "_NET_WM_NAME"), utf8, 8, 3, "lwm");
        xcb_flush(conn.get());
        thread = std::jthread(
            [=, this](std::stop_token stop)
            {
                while (!stop.stop_requested())
                {
                    auto* event = xcb_poll_for_event(conn.get());
                    if (!event)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    auto const& message = reinterpret_cast<xcb_client_message_event_t const&>(*event);
                    if ((event->response_type & ~0x80) == XCB_CLIENT_MESSAGE && message.type == command)
                    {
                        auto requester = message.data.data32[0];
                        {
                            std::lock_guard lock(mutex);
                            requests.push_back(get_window_property_string(conn.get(), requester, command).value_or(""));
                        }
                        if (answer == Answer::Reply)
                            xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, requester, response, utf8, 8, reply.size(), reply.data());
                        if (answer == Answer::Exit)
                            xcb_destroy_window(conn.get(), window);
                        xcb_flush(conn.get());
                    }
                    free(event);
                }
            }
        );
    }

    std::vector<std::string> received()
    {
        std::lock_guard lock(mutex);
        return requests;
    }
};

bool display_available() { return X11TestEnvironment::instance().available(); }
}

TEST_CASE("lwmctl prints query values, stays silent for actions and reports errors", "[ipc][lwmctl]")
{
    if (!display_available())
        SKIP("X11 unavailable");
    std::string reply;
    int expected_exit = 1;
    std::string output;
    SECTION("empty") { }
    SECTION("unknown envelope") { reply = "1.0"; }
    SECTION("error") { reply = "error busy"; }
    SECTION("success without value")
    {
        reply = "ok";
        expected_exit = 0;
    }
    SECTION("success with value")
    {
        reply = "ok 1.0";
        expected_exit = 0;
        output = "1.0\n";
    }
    FakeWm wm(reply);
    auto result = run_command(lwmctl_executable_path(), { "version" });
    REQUIRE(result);
    CHECK(result->exit_code == expected_exit);
    CHECK(result->stdout_text == output);
    if (expected_exit)
        CHECK_FALSE(result->stderr_text.empty());
    if (reply == "error busy")
        CHECK(result->stderr_text == "lwmctl: busy\n");
    CHECK(wm.received() == std::vector<std::string>{ "version" });
}

TEST_CASE("lwmctl distinguishes a missing, silent and exiting WM", "[ipc][lwmctl]")
{
    if (!display_available())
        SKIP("X11 unavailable");
    SECTION("no WM owns the screen")
    {
        auto result = run_command(lwmctl_executable_path(), { "version" });
        REQUIRE(result);
        CHECK(result->exit_code == 1);
        CHECK(result->stderr_text == "lwmctl: lwm is not running\n");
    }
    SECTION("another WM owns the screen")
    {
        FakeWm wm("ok", FakeWm::Answer::Foreign);
        auto result = run_command(lwmctl_executable_path(), { "version" });
        REQUIRE(result);
        CHECK(result->exit_code == 1);
        CHECK(result->stderr_text == "lwmctl: lwm is not running\n");
        CHECK(wm.received().empty());
    }
    SECTION("a silent WM times out with an unknown outcome")
    {
        FakeWm wm("", FakeWm::Answer::Silence);
        auto started = std::chrono::steady_clock::now();
        auto result = run_command(lwmctl_executable_path(), { "--timeout", "100", "version" });
        REQUIRE(result);
        CHECK(result->exit_code == 1);
        CHECK(result->stderr_text.find("timed out") != std::string::npos);
        CHECK(result->stderr_text.find("may have run") != std::string::npos);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    }
    SECTION("a WM that exits before replying is reported at once")
    {
        FakeWm wm("", FakeWm::Answer::Exit);
        auto started = std::chrono::steady_clock::now();
        auto result = run_command(lwmctl_executable_path(), { "--timeout", "5000", "version" });
        REQUIRE(result);
        CHECK(result->exit_code == 1);
        CHECK(result->stderr_text == "lwmctl: lwm exited before replying\n");
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    }
}

TEST_CASE("lwmctl offers local help and preserves option-like names after double dash", "[ipc][lwmctl]")
{
    auto help = run_command(lwmctl_executable_path(), { "workspace", "--help" });
    REQUIRE(help);
    CHECK(help->exit_code == 0);
    CHECK(help->stderr_text.empty());
    CHECK(help->stdout_text.find("workspace switch N") != std::string::npos);
    CHECK(help->stdout_text.find("scratchpad stash") == std::string::npos);
    if (!display_available())
        SKIP("X11 unavailable");
    FakeWm wm("ok");
    auto result = run_command(lwmctl_executable_path(), { "--", "scratchpad", "toggle", "--help" });
    REQUIRE(result);
    CHECK(result->exit_code == 0);
    CHECK(wm.received() == std::vector<std::string>{ "scratchpad toggle --help" });
}

