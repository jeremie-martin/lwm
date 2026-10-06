#include "lwm/core/types.hpp"
#include "wm_observations.hpp"
#include <catch2/catch_test_macros.hpp>
#include <xcb/xtest.h>

using namespace lwm::test;
namespace {
constexpr auto timeout = std::chrono::seconds(2);
uint8_t grab(X11Connection& conn)
{
    auto* reply = xcb_grab_pointer_reply(
        conn.get(),
        xcb_grab_pointer(
            conn.get(),
            0,
            conn.root(),
            XCB_EVENT_MASK_POINTER_MOTION,
            XCB_GRAB_MODE_ASYNC,
            XCB_GRAB_MODE_ASYNC,
            XCB_NONE,
            XCB_NONE,
            XCB_CURRENT_TIME
        ),
        nullptr
    );
    REQUIRE(reply);
    auto status = reply->status;
    free(reply);
    return status;
}

void expect_released(X11Connection& conn)
{
    REQUIRE(grab(conn) == XCB_GRAB_STATUS_SUCCESS);
    xcb_ungrab_pointer(conn.get(), XCB_CURRENT_TIME);
    xcb_flush(conn.get());
}

xcb_window_t floating_window(X11Connection& conn)
{
    auto window = create_window(conn, 100, 100, 300, 200);
    set_window_type(conn, window, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    return window;
}
}

TEST_CASE("Integration: EWMH resize preserves the opposite edges in every direction", "[integration][drag]")
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    auto window = floating_window(conn);
    auto start = require_window_geometry(conn, window);
    struct Case
    {
        uint32_t direction;
        int dx, dy, dw, dh;
    };
    for (auto test : {
             Case{ 0, 20, 30, -20, -30 },
             Case{ 1,  0, 30,   0, -30 },
             Case{ 2,  0, 30,  20, -30 },
             Case{ 3,  0,  0,  20,   0 },
             Case{ 4,  0,  0,  20,  30 },
             Case{ 5,  0,  0,   0,  30 },
             Case{ 6, 20,  0, -20,  30 },
             Case{ 7, 20,  0, -20,   0 }
    })
    {
        CAPTURE(test.direction);
        send_client_message(
            conn,
            window,
            intern_atom(conn.get(), "_NET_MOVERESIZE_WINDOW"),
            (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11),
            start.x,
            start.y,
            start.width,
            start.height
        );
        observe_title_after_events(conn, window);
        xcb_test_fake_input(conn.get(), XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, conn.root(), 100, 100, 0);
        send_client_message(conn, window, intern_atom(conn.get(), "_NET_WM_MOVERESIZE"), 100, 100, test.direction, 1);
        observe_title_after_events(conn, window);
        REQUIRE(grab(conn) == XCB_GRAB_STATUS_ALREADY_GRABBED);
        xcb_test_fake_input(conn.get(), XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, conn.root(), 120, 130, 0);
        WindowGeometry expected{ static_cast<int16_t>(start.x + test.dx),
                                 static_cast<int16_t>(start.y + test.dy),
                                 static_cast<uint16_t>(start.width + test.dw),
                                 static_cast<uint16_t>(start.height + test.dh) };
        observe_title_after_events(conn, window);
        CHECK(require_window_geometry(conn, window) == expected);
        send_pointer_event(conn, XCB_BUTTON_RELEASE, 120, 130);
        observe_title_after_events(conn, window);
        expect_released(conn);
    }
}

TEST_CASE("Integration: pointer ownership and release belong to one drag", "[integration][drag]")
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    auto window = floating_window(conn);
    auto other = floating_window(conn);
    auto start = require_window_geometry(conn, window);
    auto atom = intern_atom(conn.get(), "_NET_WM_MOVERESIZE");
    REQUIRE(grab(conn) == XCB_GRAB_STATUS_SUCCESS);
    send_client_message(conn, window, atom, 100, 100, 8, 1);
    observe_title_after_events(conn, window);
    send_pointer_event(conn, XCB_MOTION_NOTIFY, 160, 170);
    observe_title_after_events(conn, window);
    CHECK(require_window_geometry(conn, window) == start);
    CHECK(wait_for_active_window(conn, other, timeout));
    xcb_ungrab_pointer(conn.get(), XCB_CURRENT_TIME);
    send_client_message(conn, window, atom, 100, 100, 8, 1);
    observe_title_after_events(conn, window);
    REQUIRE(grab(conn) == XCB_GRAB_STATUS_ALREADY_GRABBED);
    send_client_message(conn, other, atom, 100, 100, 11); // Cannot cancel another window's interaction.
    send_pointer_event(conn, XCB_BUTTON_RELEASE, 110, 110, 3); // Nor can another button release it.
    observe_title_after_events(conn, window);
    CHECK(grab(conn) == XCB_GRAB_STATUS_ALREADY_GRABBED);
    send_pointer_event(conn, XCB_BUTTON_RELEASE, 160, 170, 1); // Apply the final position, even without motion.
    observe_title_after_events(conn, window);
    CHECK(
        require_window_geometry(conn, window)
        == WindowGeometry{ static_cast<int16_t>(start.x + 60),
                           static_cast<int16_t>(start.y + 70),
                           start.width,
                           start.height }
    );
    expect_released(conn);
}

TEST_CASE("Integration: invalidating a dragged window releases the pointer", "[integration][drag]")
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    auto marker = floating_window(conn);
    auto window = floating_window(conn);
    send_client_message(conn, window, intern_atom(conn.get(), "_NET_WM_MOVERESIZE"), 100, 100, 8, 1);
    observe_title_after_events(conn, marker);
    REQUIRE(grab(conn) == XCB_GRAB_STATUS_ALREADY_GRABBED);
    SECTION("destroy") { destroy_window(conn, window); }
    SECTION("fullscreen")
    {
        send_client_message(
            conn,
            window,
            intern_atom(conn.get(), "_NET_WM_STATE"),
            1,
            intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN")
        );
    }
    SECTION("workspace") { send_client_message(conn, conn.root(), intern_atom(conn.get(), "_NET_CURRENT_DESKTOP"), 1); }
    observe_title_after_events(conn, marker);
    expect_released(conn);
}

TEST_CASE("Integration: tiled drops translate visible slots past iconic members", "[integration][drag]")
{
    auto env = TestEnvironment::create("[appearance]\npadding = 10\nborder_width = 1\n[mousebinds]\n\"super+1\" = \"drag_window\"\n\"super+3\" = \"resize_floating\"\n");
    REQUIRE(env);
    auto& conn = env->conn;
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 3; ++i)
    {
        windows.push_back(create_window(conn, 10, 10, 200, 200));
        map_window(conn, windows.back());
        REQUIRE(wait_for_active_window(conn, windows.back(), timeout));
    }
    // Establish actual layout order independently of insertion policy.
    std::ranges::sort(
        windows,
        [&](auto a, auto b)
        {
            auto x = require_window_geometry(conn, a), y = require_window_geometry(conn, b);
            return x.x < y.x || (x.x == y.x && x.y < y.y);
        }
    );
    auto hidden = windows[1];
    stash(hidden);
    observe_title_after_events(conn, windows[0]);
    SECTION("with a sticky guest from another workspace")
    {
        auto guest = create_window(conn, 10, 10, 200, 200);
        map_window(conn, guest);
        REQUIRE(wait_for_active_window(conn, guest, timeout));
        send_client_message(conn, guest, intern_atom(conn.get(), "_NET_WM_DESKTOP"), 1);
        send_client_message(
            conn,
            guest,
            intern_atom(conn.get(), "_NET_WM_STATE"),
            1,
            intern_atom(conn.get(), "_NET_WM_STATE_STICKY")
        );
        observe_title_after_events(conn, windows[0]);
        REQUIRE(require_window_geometry(conn, guest).x > require_window_geometry(conn, windows[0]).x);
    }
    SECTION("without sticky guests") { }
    auto left = require_window_geometry(conn, windows[0]);
    auto right = require_window_geometry(conn, windows[2]);
    REQUIRE(left.x < right.x);
    send_pointer_event(conn, XCB_BUTTON_PRESS, left.x + 30, left.y + 30, 1, windows[0], XCB_MOD_MASK_4);
    observe_title_after_events(conn, windows[0]);
    REQUIRE(grab(conn) == XCB_GRAB_STATUS_ALREADY_GRABBED);
    send_pointer_event(conn, XCB_BUTTON_RELEASE, right.x + right.width / 2, right.y + right.height / 4);
    observe_title_after_events(conn, windows[0]);
    CHECK(require_window_geometry(conn, windows[2]).x < require_window_geometry(conn, windows[0]).x);
    CHECK(require_window_geometry(conn, hidden).x == lwm::OFF_SCREEN_X);
    expect_released(conn);
}

TEST_CASE("Integration: cancelling a tiled preview restores layout without reordering", "[integration][drag]")
{
    auto env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n[mousebinds]\n\"super+1\" = \"drag_window\"\n\"super+3\" = \"resize_floating\"\n");
    REQUIRE(env);
    auto& conn = env->conn;
    auto window = create_window(conn, 10, 10, 200, 200);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    auto start = require_window_geometry(conn, window);
    send_pointer_event(conn, XCB_BUTTON_PRESS, 100, 100, 1, window, XCB_MOD_MASK_4);
    observe_title_after_events(conn, window);
    send_pointer_event(conn, XCB_MOTION_NOTIFY, 140, 150);
    observe_title_after_events(conn, window);
    REQUIRE(require_window_geometry(conn, window).x == start.x + 40);
    REQUIRE(require_window_geometry(conn, window).y == start.y + 50);
    SECTION("explicit cancel")
    {
        send_client_message(conn, window, intern_atom(conn.get(), "_NET_WM_MOVERESIZE"), 140, 150, 11);
    }
    SECTION("configuration reload")
    {
        REQUIRE(send_ipc_command("reload-config") == "ok");
    }
    observe_title_after_events(conn, window);
    CHECK(require_window_geometry(conn, window) == start);
    expect_released(conn);
}

TEST_CASE("Integration: split resize ends when its participants change", "[integration][drag]")
{
    auto env = TestEnvironment::create("[appearance]\npadding = 10\nborder_width = 1\n[mousebinds]\n\"super+1\" = \"drag_window\"\n\"super+3\" = \"resize_floating\"\n");
    REQUIRE(env);
    auto& conn = env->conn;
    auto first = create_window(conn, 10, 10, 200, 200), second = create_window(conn, 10, 10, 200, 200);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, timeout));
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, timeout));
    auto left = require_window_geometry(conn, first), right = require_window_geometry(conn, second);
    if (left.x > right.x)
        std::swap(left, right);
    auto x = static_cast<int16_t>((left.x + left.width + right.x) / 2);
    auto y = static_cast<int16_t>(left.y + left.height / 2);
    send_pointer_event(conn, XCB_BUTTON_PRESS, x, y);
    observe_title_after_events(conn, first);
    REQUIRE(grab(conn) == XCB_GRAB_STATUS_ALREADY_GRABBED);
    send_client_message(conn, second, intern_atom(conn.get(), "_NET_WM_DESKTOP"), 1);
    observe_title_after_events(conn, first);
    expect_released(conn);
    auto settled = require_window_geometry(conn, first);
    send_pointer_event(conn, XCB_MOTION_NOTIFY, x + 100, y);
    observe_title_after_events(conn, first);
    CHECK(require_window_geometry(conn, first) == settled);
}

TEST_CASE("Integration: real button grabs drive floating resize and tiled conversion", "[integration][drag][input]")
{
    auto env = TestEnvironment::create(R"(
[mousebinds]
"3" = "resize_floating")");
    REQUIRE(env);
    auto& conn = env->conn;
    xcb_window_t window = XCB_NONE;
    SECTION("floating") { window = floating_window(conn); }
    SECTION("maximized floating")
    {
        window = floating_window(conn);
        send_client_message(
            conn,
            window,
            intern_atom(conn.get(), "_NET_WM_STATE"),
            1,
            intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_HORZ"),
            intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_VERT")
        );
        observe_title_after_events(conn, window);
    }
    SECTION("tiled away from a split")
    {
        window = create_window(conn, 10, 10, 200, 200);
        map_window(conn, window);
        REQUIRE(wait_for_active_window(conn, window, timeout));
    }
    REQUIRE(window != XCB_NONE);
    auto start = require_window_geometry(conn, window);
    int16_t x = start.x + start.width / 2, y = start.y + start.height / 2;
    xcb_test_fake_input(conn.get(), XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, conn.root(), x, y, 0);
    xcb_test_fake_input(conn.get(), XCB_BUTTON_PRESS, 3, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    observe_title_after_events(conn, window);
    REQUIRE(grab(conn) == XCB_GRAB_STATUS_ALREADY_GRABBED);
    xcb_test_fake_input(conn.get(), XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, conn.root(), x + 30, y + 20, 0);
    observe_title_after_events(conn, window);
    CHECK(
        require_window_geometry(conn, window)
        == WindowGeometry{ start.x,
                           start.y,
                           static_cast<uint16_t>(start.width + 30),
                           static_cast<uint16_t>(start.height + 20) }
    );
    xcb_test_fake_input(conn.get(), XCB_BUTTON_RELEASE, 3, XCB_CURRENT_TIME, conn.root(), 0, 0, 0);
    observe_title_after_events(conn, window);
    expect_released(conn);
    CHECK(get_window_property_string(conn.get(), window, intern_atom(conn.get(), "_LWM_WINDOW_CLASS")) == "floating");
}

TEST_CASE("Integration: floating motion bursts stop at release and use its final coordinates", "[integration][drag]")
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    auto window = floating_window(conn);
    auto start = require_window_geometry(conn, window);
    send_client_message(conn, window, intern_atom(conn.get(), "_NET_WM_MOVERESIZE"), 100, 100, 8, 1);
    observe_title_after_events(conn, window);
    REQUIRE(grab(conn) == XCB_GRAB_STATUS_ALREADY_GRABBED);
    for (int16_t x = 101; x <= 150; ++x) send_pointer_event(conn, XCB_MOTION_NOTIFY, x, 100);
    send_pointer_event(conn, XCB_BUTTON_RELEASE, 160, 170);
    send_pointer_event(conn, XCB_MOTION_NOTIFY, 300, 300);
    observe_title_after_events(conn, window);
    CHECK(
        require_window_geometry(conn, window)
        == WindowGeometry{ static_cast<int16_t>(start.x + 60),
                           static_cast<int16_t>(start.y + 70),
                           start.width,
                           start.height }
    );
    expect_released(conn);
}

TEST_CASE("Integration: retained maximize flags do not cancel a tiled move", "[integration][drag]")
{
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    auto window = create_window(conn, 10, 10, 200, 200);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, timeout));
    send_client_message(
        conn,
        window,
        intern_atom(conn.get(), "_NET_WM_STATE"),
        1,
        intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_HORZ"),
        intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_VERT")
    );
    observe_title_after_events(conn, window);
    REQUIRE(get_window_property_string(conn.get(), window, intern_atom(conn.get(), "_LWM_WINDOW_CLASS")) == "tiled");
    auto start = require_window_geometry(conn, window);
    send_pointer_event(conn, XCB_BUTTON_PRESS, 100, 100, 1, window, XCB_MOD_MASK_4);
    observe_title_after_events(conn, window);
    send_pointer_event(conn, XCB_MOTION_NOTIFY, 140, 150);
    observe_title_after_events(conn, window);
    CHECK(grab(conn) == XCB_GRAB_STATUS_ALREADY_GRABBED);
    CHECK(require_window_geometry(conn, window).x == start.x + 40);
    CHECK(require_window_geometry(conn, window).y == start.y + 50);
    send_pointer_event(conn, XCB_BUTTON_RELEASE, 140, 150);
    observe_title_after_events(conn, window);
    CHECK(require_window_geometry(conn, window) == start);
    expect_released(conn);
}

TEST_CASE("Integration: a desktop window under the pointer does not hide split borders", "[integration][drag]")
{
    auto env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n[appearance]\npadding = 10\nborder_width = 1\n[mousebinds]\n\"super+1\" = \"drag_window\"\n\"super+3\" = \"resize_floating\"\n");
    REQUIRE(env);
    auto& conn = env->conn;
    auto desktop = create_window(conn, 0, 0, 4000, 4000);
    set_window_type(conn, desktop, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DESKTOP"));
    map_window(conn, desktop);
    REQUIRE(wait_for_condition(
        [&]
        {
            return get_window_property_string(conn.get(), desktop, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"))
                == "desktop";
        },
        timeout
    ));
    // Desktop windows carry no workspace, so every workspace must treat them alike.
    SECTION("first workspace") { }
    SECTION("second workspace")
    {
        send_client_message(conn, conn.root(), intern_atom(conn.get(), "_NET_CURRENT_DESKTOP"), 1);
    }
    auto first = create_window(conn, 10, 10, 200, 200), second = create_window(conn, 10, 10, 200, 200);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, timeout));
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, timeout));
    auto left = require_window_geometry(conn, first), right = require_window_geometry(conn, second);
    if (left.x > right.x)
        std::swap(left, right);
    auto x = static_cast<int16_t>((left.x + left.width + right.x) / 2);
    auto y = static_cast<int16_t>(left.y + left.height / 2);
    // The root grab reports the desktop window as the child under the split border.
    send_pointer_event(conn, XCB_BUTTON_PRESS, x, y, 3, desktop, XCB_MOD_MASK_4);
    observe_title_after_events(conn, first);
    CHECK(grab(conn) == XCB_GRAB_STATUS_ALREADY_GRABBED);
    send_pointer_event(conn, XCB_BUTTON_RELEASE, x, y, 3);
    observe_title_after_events(conn, first);
    expect_released(conn);
}
