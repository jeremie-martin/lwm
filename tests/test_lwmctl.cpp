#include "state_watch.hpp"
#include <catch2/catch_test_macros.hpp>
#include <mutex>
#include <thread>

using namespace lwm::test;

namespace {
// Owns WM_S0 on a display without a WM and answers each command with a scripted
// reply, exercising the real CLI's protocol independently of LWM.
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

TEST_CASE("lwmctl watch waits through foreign ownership and attaches to the next LWM", "[ipc][lwmctl][watch]")
{
    if (!display_available())
        SKIP("X11 unavailable");
    FakeWm wm("ok");
    auto& conn = wm.conn;
    auto utf8 = intern_atom(conn.get(), "UTF8_STRING");
    auto state = intern_atom(conn.get(), "_LWM_STATE");
    auto name = intern_atom(conn.get(), "_NET_WM_NAME");
    auto selection = intern_atom(conn.get(), "WM_S0");
    auto publish = [&](xcb_window_t window, char const* text)
    {
        xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, state, utf8, 8, std::strlen(text), text);
        xcb_flush(conn.get());
    };
    auto announce = [&](xcb_window_t window, bool message = true)
    {
        xcb_set_selection_owner(conn.get(), window, selection, XCB_CURRENT_TIME);
        if (!message)
            return;
        xcb_client_message_event_t event{};
        event.response_type = XCB_CLIENT_MESSAGE;
        event.format = 32;
        event.window = conn.root();
        event.type = intern_atom(conn.get(), "MANAGER");
        event.data.data32[1] = selection;
        event.data.data32[2] = window;
        xcb_send_event(
            conn.get(),
            0,
            conn.root(),
            XCB_EVENT_MASK_STRUCTURE_NOTIFY,
            reinterpret_cast<char const*>(&event)
        );
        xcb_flush(conn.get());
    };
    publish(wm.window, "{\"lifetime\":1}");
    Watcher watcher;
    CHECK(watcher.state().at("lifetime") == 1);
    auto foreign = create_window(conn, 0, 0, 1, 1);
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, foreign, name, utf8, 8, 7, "foreign");
    publish(foreign, "{\"lifetime\":2}");
    SECTION("MANAGER announcement") { announce(foreign); }
    SECTION("Old owner destruction")
    {
        announce(foreign, false);
        xcb_destroy_window(conn.get(), wm.window);
        xcb_flush(conn.get());
    }
    CHECK_FALSE(watcher.line(std::chrono::milliseconds(200)));
    publish(foreign, "{\"lifetime\":3}");
    CHECK_FALSE(watcher.line(std::chrono::milliseconds(200)));
    auto successor = create_window(conn, 0, 0, 1, 1);
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, successor, name, utf8, 8, 3, "lwm");
    publish(successor, "{\"lifetime\":4}");
    announce(successor);
    CHECK(watcher.state().at("lifetime") == 4);
    destroy_window(conn, foreign);
    destroy_window(conn, successor);
}
