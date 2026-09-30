#include "x11_test_harness.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <sstream>
#include <thread>
#include <xcb/xcb_icccm.h>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

bool has_wm_hints_urgency(xcb_connection_t* conn, xcb_window_t window)
{
    constexpr uint32_t XUrgencyHint = 256;
    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn, xcb_icccm_get_wm_hints(conn, window), &hints, nullptr))
        return (hints.flags & XUrgencyHint) != 0;
    return false;
}

void set_wm_hints_urgency(xcb_connection_t* conn, xcb_window_t window, bool urgent)
{
    constexpr uint32_t XUrgencyHint = 256;
    xcb_icccm_wm_hints_t hints = {};
    hints.flags = XCB_ICCCM_WM_HINT_INPUT;
    hints.input = 1;
    if (urgent)
        hints.flags |= XUrgencyHint;
    xcb_icccm_set_wm_hints(conn, window, &hints);
    xcb_flush(conn);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// notify-attention IPC command tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Integration: notify-attention sets urgency by exact window ID even when names are ambiguous",
    "[integration][notify_attention]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    set_window_wm_class(conn, w1, "ghostty", "Ghostty");
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    set_window_wm_class(conn, w2, "ghostty", "Ghostty");
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    std::string window_arg = "window=" + std::to_string(w1);
    auto result = run_lwmctl(wm, { "notify-attention", window_arg });
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);

    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));
    REQUIRE(wait_for_condition([&]() { return has_wm_hints_urgency(conn.get(), w1); }, kTimeout));

    REQUIRE_FALSE(property_has_atom(conn.get(), w2, net_wm_state, net_wm_state_demands_attention));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: notify-attention window=<active> is skipped", "[integration][notify_attention]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // The user is already focused on the window — no urgency needed.
    std::string window_arg = "window=" + std::to_string(w1);
    auto result = run_lwmctl(wm, { "notify-attention", window_arg });
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->stdout_text.find("skipped-active") != std::string::npos);
    REQUIRE_FALSE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));

    destroy_window(conn, w1);
}

TEST_CASE("Integration: notify-attention clears on focus", "[integration][notify_attention]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // Mark w1 urgent
    std::string window_arg = "window=" + std::to_string(w1);
    run_lwmctl(wm, { "notify-attention", window_arg });

    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));

    // Focus w1 — both EWMH and ICCCM urgency should clear
    send_client_message(conn, w1, net_active_window, 2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));
    REQUIRE(wait_for_condition([&]() { return !has_wm_hints_urgency(conn.get(), w1); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: notify-attention returns no-match for unmanaged window ID", "[integration][notify_attention]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    auto result = run_lwmctl(wm, { "notify-attention", "window=0" });
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->stdout_text.find("no-match") != std::string::npos);
    REQUIRE_FALSE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));
    REQUIRE_FALSE(has_wm_hints_urgency(conn.get(), w1));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: notify-attention rejects extra tokens after window=<xid>", "[integration][notify_attention]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    std::string arg = "window=" + std::to_string(w1) + " app-name=Ghostty";
    auto result = send_ipc_command(*socket, "notify-attention " + arg);
    REQUIRE(result);
    REQUIRE(result->starts_with("error "));
    REQUIRE_FALSE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: notify-attention accepts hex window id", "[integration][notify_attention]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    std::ostringstream hex_arg;
    hex_arg << "window=0x" << std::hex << w1;
    auto result = run_lwmctl(wm, { "notify-attention", hex_arg.str() });
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: WM-initiated urgency survives app WM_HINTS rewrite",
    "[integration][notify_attention][wm_hints]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // Mark w1 urgent via WM IPC (WM-initiated urgency).
    std::string window_arg = "window=" + std::to_string(w1);
    run_lwmctl(wm, { "notify-attention", window_arg });
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));

    // App rewrites WM_HINTS (e.g. changing window group) WITHOUT urgency.
    // This should NOT clear the WM-initiated urgency.
    xcb_icccm_wm_hints_t hints = {};
    hints.flags = XCB_ICCCM_WM_HINT_INPUT;
    hints.input = 1;
    xcb_icccm_set_wm_hints(conn.get(), w1, &hints);
    xcb_flush(conn.get());

    // Give the WM time to process the PropertyNotify.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Urgency should still be present.
    REQUIRE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));
    REQUIRE(has_wm_hints_urgency(conn.get(), w1));

    // Focusing w1 should still clear urgency as before.
    send_client_message(conn, w1, net_active_window, 2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: WM-initiated urgency survives app _NET_WM_STATE remove and toggle",
    "[integration][notify_attention][wm_state]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    std::string window_arg = "window=" + std::to_string(w1);
    run_lwmctl(wm, { "notify-attention", window_arg });
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));

    send_client_message(conn, w1, net_wm_state, 0, net_wm_state_demands_attention, 0, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));
    REQUIRE(has_wm_hints_urgency(conn.get(), w1));

    send_client_message(conn, w1, net_wm_state, 2, net_wm_state_demands_attention, 0, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));
    REQUIRE(has_wm_hints_urgency(conn.get(), w1));

    send_client_message(conn, w1, net_active_window, 2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));
    REQUIRE(wait_for_condition([&]() { return !has_wm_hints_urgency(conn.get(), w1); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: app-only urgency clears on app WM_HINTS rewrite", "[integration][notify_attention][wm_hints]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    set_wm_hints_urgency(conn.get(), w1, true);
    REQUIRE(wait_for_condition(
        [&]()
        {
            return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention)
                && has_wm_hints_urgency(conn.get(), w1);
        },
        kTimeout
    ));

    set_wm_hints_urgency(conn.get(), w1, false);
    REQUIRE(wait_for_condition(
        [&]()
        {
            return !property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention)
                && !has_wm_hints_urgency(conn.get(), w1);
        },
        kTimeout
    ));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: app-only urgency survives restart as app-owned state",
    "[integration][notify_attention][restart][wm_state]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    set_wm_hints_urgency(conn.get(), w1, true);
    REQUIRE(wait_for_condition(
        [&]()
        {
            return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention)
                && has_wm_hints_urgency(conn.get(), w1);
        },
        kTimeout
    ));

    auto previous_wm = supporting_wm_window(conn);
    REQUIRE(previous_wm.has_value());
    auto restart_result = run_lwmctl(wm, { "restart" });
    (void)restart_result;
    REQUIRE(wait_for_wm_ready(conn, std::chrono::seconds(5), *previous_wm));
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    REQUIRE(wait_for_condition(
        [&]()
        {
            return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention)
                && has_wm_hints_urgency(conn.get(), w1);
        },
        kTimeout
    ));

    send_client_message(conn, w1, net_wm_state, 0, net_wm_state_demands_attention, 0, 0, 0);
    REQUIRE(wait_for_condition(
        [&]()
        {
            return !property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention)
                && !has_wm_hints_urgency(conn.get(), w1);
        },
        kTimeout
    ));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: WM-initiated urgency survives restart and later WM_HINTS rewrites",
    "[integration][notify_attention][restart][wm_hints]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto& wm = test_env->wm;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_demands_attention = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    std::string window_arg = "window=" + std::to_string(w1);
    auto notify_result = run_lwmctl(wm, { "notify-attention", window_arg });
    REQUIRE(notify_result.has_value());
    REQUIRE(notify_result->exit_code == 0);
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));
    REQUIRE(wait_for_condition([&]() { return has_wm_hints_urgency(conn.get(), w1); }, kTimeout));

    xcb_atom_t supporting = intern_atom(conn.get(), "_NET_SUPPORTING_WM_CHECK");
    auto old_supporting = get_window_property_window(conn.get(), conn.root(), supporting);
    REQUIRE(old_supporting.has_value());

    auto restart_result = run_lwmctl(wm, { "restart" });
    (void)restart_result;
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto current = get_window_property_window(conn.get(), conn.root(), supporting);
            return current && *current != XCB_NONE && *current != *old_supporting;
        },
        std::chrono::seconds(5)
    ));

    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));
    REQUIRE(wait_for_condition([&]() { return has_wm_hints_urgency(conn.get(), w1); }, kTimeout));

    xcb_icccm_wm_hints_t hints = {};
    hints.flags = XCB_ICCCM_WM_HINT_INPUT;
    hints.input = 1;
    xcb_icccm_set_wm_hints(conn.get(), w1, &hints);
    xcb_flush(conn.get());
    REQUIRE(wait_for_condition(
        [&]()
        {
            return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention)
                && has_wm_hints_urgency(conn.get(), w1);
        },
        kTimeout
    ));

    send_client_message(conn, w1, net_active_window, 2);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(wait_for_condition(
        [&]() { return !property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}
