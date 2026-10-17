#include "wm_observations.hpp"
#include "lwm/core/xproperty.hpp"
#include <catch2/generators/catch_generators.hpp>

using namespace lwm::test;

namespace {
constexpr auto timeout = std::chrono::seconds(2);

// A small toolkit stand-in: it observes configurations and answers an allocation
// below its content minimum with a resize and updated normal-size hints. Keeping
// the answer queued models a toolkit dispatching it after a later WM operation.
class ResponsiveClient
{
    X11Connection& connection_;
    xcb_window_t window_;
    bool pending_minimum_ = false;

public:
    unsigned undersized_configurations = 0;
    static constexpr uint16_t minimum_width = 122, minimum_height = 38;

    ResponsiveClient(X11Connection& connection, xcb_window_t window)
        : connection_(connection), window_(window)
    {
        uint32_t events = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(connection_.get(), window_, XCB_CW_EVENT_MASK, &events);
        xcb_flush(connection_.get());
    }

    void collect()
    {
        // Commands have already completed their effects. This round trip brings
        // their notifications onto this application's connection before draining.
        auto sync = lwm::reply(xcb_get_input_focus_reply(
            connection_.get(), xcb_get_input_focus(connection_.get()), nullptr));
        REQUIRE(sync);
        while (auto event = lwm::reply(xcb_poll_for_event(connection_.get())))
        {
            if ((event->response_type & ~0x80) != XCB_CONFIGURE_NOTIFY)
                continue;
            auto const& configuration = reinterpret_cast<xcb_configure_notify_event_t const&>(*event);
            if (configuration.window == window_
                && (configuration.width < minimum_width || configuration.height < minimum_height))
            {
                ++undersized_configurations;
                pending_minimum_ = true;
            }
        }
    }

    void resize_to_minimum()
    {
        uint32_t size[] = { minimum_width, minimum_height };
        xcb_configure_window(connection_.get(), window_, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
        xcb_size_hints_t hints{};
        xcb_icccm_size_hints_set_size(&hints, false, minimum_width, minimum_height);
        xcb_icccm_set_wm_normal_hints(connection_.get(), window_, &hints);
        xcb_flush(connection_.get());
    }

    void dispatch_delayed_feedback()
    {
        if (std::exchange(pending_minimum_, false))
            resize_to_minimum();
        observe_title_after_events(connection_, window_);
    }
};
}

TEST_CASE("Integration: hiding does not provoke delayed application resize feedback",
          "[integration][feedback][geometry][fullscreen][workspace][scratchpad]")
{
    auto hiding = GENERATE("sibling fullscreen", "workspace", "scratchpad pool");
    CAPTURE(hiding);
    auto env = TestEnvironment::create("[workspaces]\nnames = ['one', 'two']\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& connection = env->conn;
    auto peer = create_window(connection, 20, 30, 400, 300);
    map_window(connection, peer);
    REQUIRE(wait_for_active_window(connection, peer, timeout));
    auto window = create_window(connection, 60, 70, 800, 600);
    set_window_type(connection, window, intern_atom(connection.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    ResponsiveClient application(connection, window);
    map_window(connection, window);
    REQUIRE(wait_for_active_window(connection, window, timeout));
    auto original = get_window_geometry(connection, window);
    REQUIRE(original);
    application.collect();
    REQUIRE(application.undersized_configurations == 0);

    if (std::string_view(hiding) == "sibling fullscreen")
    {
        ipc_ok("window focus " + std::to_string(peer));
        ipc_ok("window fullscreen");
    }
    else if (std::string_view(hiding) == "workspace")
        ipc_ok("workspace switch 1");
    else
        stash(window);
    application.collect();

    if (std::string_view(hiding) == "sibling fullscreen")
        ipc_ok("window fullscreen");
    else if (std::string_view(hiding) == "workspace")
        ipc_ok("workspace switch 0");
    else
        ipc_ok("scratchpad cycle");
    application.collect();
    application.dispatch_delayed_feedback();

    auto restored = get_window_geometry(connection, window);
    REQUIRE(restored);
    CAPTURE(original->x, original->y, original->width, original->height,
            restored->x, restored->y, restored->width, restored->height,
            application.undersized_configurations);
    CHECK(restored == original);
    CHECK(application.undersized_configurations == 0);
    destroy_window(connection, window);
    destroy_window(connection, peer);
}

TEST_CASE("Integration: legitimate application size feedback while hidden survives revealing",
          "[integration][feedback][geometry][workspace]")
{
    auto env = TestEnvironment::create("[workspaces]\nnames = ['one', 'two']\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& connection = env->conn;
    auto window = create_window(connection, 60, 70, 800, 600);
    set_window_type(connection, window, intern_atom(connection.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    ResponsiveClient application(connection, window);
    map_window(connection, window);
    REQUIRE(wait_for_active_window(connection, window, timeout));
    auto original = get_window_geometry(connection, window);
    REQUIRE(original);
    ipc_ok("workspace switch 1");

    application.resize_to_minimum();
    observe_title_after_events(connection, window);
    ipc_ok("workspace switch 0");
    auto resized = get_window_geometry(connection, window);
    REQUIRE(resized);
    CHECK(resized->x == original->x);
    CHECK(resized->y == original->y);
    CHECK(resized->width == ResponsiveClient::minimum_width);
    CHECK(resized->height == ResponsiveClient::minimum_height);
    destroy_window(connection, window);
}
