#include "x11_test_harness.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <thread>
#include <xcb/xcb_icccm.h>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

struct TestEnvironment
{
    X11TestEnvironment& x11_env;
    X11Connection conn;
    LwmProcess wm;

    bool ok() const { return conn.ok() && wm.running(); }

    static std::optional<TestEnvironment> create()
    {
        auto& env = X11TestEnvironment::instance();
        if (!env.available())
        {
            WARN("Xvfb not available; set LWM_TEST_ALLOW_EXISTING_DISPLAY=1 to use an existing DISPLAY.");
            return std::nullopt;
        }

        X11Connection conn;
        if (!conn.ok())
        {
            WARN("Failed to connect to X server.");
            return std::nullopt;
        }

        LwmProcess wm(env.display());
        if (!wm.running())
        {
            WARN("Failed to start lwm.");
            return std::nullopt;
        }

        if (!wait_for_wm_ready(conn, kTimeout))
        {
            WARN("Window manager not ready.");
            return std::nullopt;
        }

        return TestEnvironment{ env, std::move(conn), std::move(wm) };
    }
};

std::vector<xcb_atom_t> get_window_property_atoms(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom)
{
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_ATOM_ATOM, 0, 64);
    auto* reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply)
        return {};

    std::vector<xcb_atom_t> result;
    auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
    int len = xcb_get_property_value_length(reply) / 4;
    result.assign(atoms, atoms + len);
    free(reply);
    return result;
}

bool property_has_atom(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t property, xcb_atom_t atom)
{
    auto atoms = get_window_property_atoms(conn, window, property);
    return std::ranges::find(atoms, atom) != atoms.end();
}

bool has_wm_hints_urgency(xcb_connection_t* conn, xcb_window_t window)
{
    constexpr uint32_t XUrgencyHint = 256;
    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn, xcb_icccm_get_wm_hints(conn, window), &hints, nullptr))
        return (hints.flags & XUrgencyHint) != 0;
    return false;
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

    // Mark w1 by exact window ID — both EWMH and ICCCM urgency.
    // The app-name is ambiguous, but window= is authoritative.
    std::string window_arg = "window=" + std::to_string(w1);
    auto result = run_lwmctl(wm, {"notify-attention", window_arg, "app-name=Ghostty"});
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);

    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]() { return has_wm_hints_urgency(conn.get(), w1); },
        kTimeout
    ));

    REQUIRE_FALSE(property_has_atom(conn.get(), w2, net_wm_state, net_wm_state_demands_attention));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: notify-attention window=<active> is skipped",
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
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // The user is already focused on the window — no urgency needed.
    std::string window_arg = "window=" + std::to_string(w1);
    auto result = run_lwmctl(wm, {"notify-attention", window_arg});
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->stdout_text.find("skipped-active") != std::string::npos);
    REQUIRE_FALSE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));

    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: notify-attention clears on focus",
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
    run_lwmctl(wm, {"notify-attention", window_arg});

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
    REQUIRE(wait_for_condition(
        [&]() { return !has_wm_hints_urgency(conn.get(), w1); },
        kTimeout
    ));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: notify-attention returns no-match for unknown app",
    "[integration][notify_attention]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");

    auto& wm = test_env->wm;

    auto result = run_lwmctl(wm, {"notify-attention", "app-name=NonExistentApp"});
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->stdout_text.find("no-match") != std::string::npos);
}

TEST_CASE(
    "Integration: notify-attention invalid exact window does not fall back to app-name",
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
    set_window_wm_class(conn, w2, "editor", "EditorApp");
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    auto result = run_lwmctl(wm, {"notify-attention", "window=0", "app-name=Ghostty"});
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->stdout_text.find("no-match") != std::string::npos);
    REQUIRE_FALSE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));
    REQUIRE_FALSE(has_wm_hints_urgency(conn.get(), w1));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: notify-attention app-name ambiguity returns no-match across workspaces",
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
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_demands_attention != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_number_of_desktops != XCB_NONE);

    uint32_t num_desktops = get_window_property_cardinal(conn.get(), conn.root(), net_number_of_desktops).value_or(0);
    if (num_desktops < 2)
        SKIP("Need at least 2 desktops");

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    set_window_wm_class(conn, w1, "ghostty", "Ghostty");
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    set_window_wm_class(conn, w2, "ghostty", "Ghostty");
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    send_client_message(conn, w2, net_wm_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), w2, net_wm_desktop, 1, kTimeout));

    auto result = run_lwmctl(wm, {"notify-attention", "app-name=Ghostty"});
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->stdout_text.find("no-match") != std::string::npos);
    REQUIRE_FALSE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));
    REQUIRE_FALSE(property_has_atom(conn.get(), w2, net_wm_state, net_wm_state_demands_attention));
    REQUIRE_FALSE(has_wm_hints_urgency(conn.get(), w1));
    REQUIRE_FALSE(has_wm_hints_urgency(conn.get(), w2));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: notify-attention app-name skips active instead of marking sibling",
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

    // Create two windows with different WM_CLASS.
    // w1 = "BrowserApp", w2 = "EditorApp".
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    set_window_wm_class(conn, w1, "browser", "BrowserApp");
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    set_window_wm_class(conn, w2, "editor", "EditorApp");
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // Notification for "EditorApp" — the active window.
    // Should return skipped-active, NOT mark w1 (which is a different app).
    auto result = run_lwmctl(wm, {"notify-attention", "app-name=EditorApp"});
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->stdout_text.find("skipped-active") != std::string::npos);
    REQUIRE_FALSE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));
    REQUIRE_FALSE(property_has_atom(conn.get(), w2, net_wm_state, net_wm_state_demands_attention));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: notify-attention desktop-entry and app-name ambiguity returns no-match",
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

    // w1: WM_CLASS instance="chromium" class="Chromium"
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    set_window_wm_class(conn, w1, "chromium", "Chromium");
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // w2: WM_CLASS instance="google-chrome" class="Google-chrome" (active)
    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    set_window_wm_class(conn, w2, "google-chrome", "Google-chrome");
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // desktop-entry and app-name each match a different managed client.  This is
    // ambiguous app metadata, not a source-window identity.
    auto result = run_lwmctl(wm, {"notify-attention", "desktop-entry=Google-chrome", "app-name=Chromium"});
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->stdout_text.find("no-match") != std::string::npos);
    REQUIRE_FALSE(property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention));
    REQUIRE_FALSE(property_has_atom(conn.get(), w2, net_wm_state, net_wm_state_demands_attention));

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
    run_lwmctl(wm, {"notify-attention", window_arg});
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
    "Integration: notify-attention accepts app-name values with spaces",
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
    set_window_wm_class(conn, w1, "code-instance", "Visual Studio Code");
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    set_window_wm_class(conn, w2, "terminal", "Terminal");
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    auto result = run_lwmctl(wm, {"notify-attention", "app-name=Visual Studio Code"});
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));
    REQUIRE_FALSE(property_has_atom(conn.get(), w2, net_wm_state, net_wm_state_demands_attention));

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
    auto notify_result = run_lwmctl(wm, {"notify-attention", window_arg});
    REQUIRE(notify_result.has_value());
    REQUIRE(notify_result->exit_code == 0);
    REQUIRE(wait_for_condition(
        [&]() { return property_has_atom(conn.get(), w1, net_wm_state, net_wm_state_demands_attention); },
        kTimeout
    ));
    REQUIRE(wait_for_condition([&]() { return has_wm_hints_urgency(conn.get(), w1); }, kTimeout));

    auto restart_result = run_lwmctl(wm, {"restart"});
    (void)restart_result;
    REQUIRE(wait_for_wm_ready(conn, std::chrono::seconds(5)));

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
