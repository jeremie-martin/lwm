/**
 * @file test_integration_wm_state.cpp
 * @brief Integration tests for _NET_WM_STATE client message handling.
 *
 * Tests the WM's response to _NET_WM_STATE add/remove/toggle requests for:
 * above, below, skip_taskbar, skip_pager, demands_attention, modal.
 * These are black-box tests: create a window, send client messages, verify
 * the resulting EWMH state atoms.
 */

#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

struct TestEnvironment
{
    X11TestEnvironment& x11_env;
    X11Connection conn;
    LwmProcess wm;

    bool ok() const { return conn.ok() && wm.running(); }

    static std::optional<TestEnvironment> create(std::string const& config = "")
    {
        auto& env = X11TestEnvironment::instance();
        if (!env.available())
        {
            WARN("Xvfb not available");
            return std::nullopt;
        }

        X11Connection conn;
        if (!conn.ok())
        {
            WARN("Failed to connect to X server");
            return std::nullopt;
        }

        LwmProcess wm(env.display(), config);
        if (!wm.running())
        {
            WARN("Failed to start lwm");
            return std::nullopt;
        }

        if (!wait_for_wm_ready(conn, kTimeout))
        {
            WARN("Window manager not ready");
            return std::nullopt;
        }

        return TestEnvironment{ env, std::move(conn), std::move(wm) };
    }
};

bool has_state(X11Connection& conn, xcb_window_t window, xcb_atom_t state)
{
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    if (net_wm_state == XCB_NONE)
        return false;

    auto cookie = xcb_get_property(conn.get(), 0, window, net_wm_state, XCB_ATOM_ATOM, 0, 16);
    auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return false;

    bool present = false;
    auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
    int len = xcb_get_property_value_length(reply) / 4;
    for (int i = 0; i < len; ++i)
    {
        if (atoms[i] == state)
        {
            present = true;
            break;
        }
    }
    free(reply);
    return present;
}

/// Send a _NET_WM_STATE client message. action: 0=remove, 1=add, 2=toggle
void send_wm_state_change(
    X11Connection& conn, xcb_window_t window, uint32_t action, xcb_atom_t state1, xcb_atom_t state2 = XCB_NONE)
{
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    send_client_message(conn, window, net_wm_state, action, state1, state2);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Above / Below
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Integration: _NET_WM_STATE add above sets state atom", "[integration][wm_state][above]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t state_above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));
    REQUIRE_FALSE(has_state(conn, w, state_above));

    send_wm_state_change(conn, w, 1, state_above);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_above); }, kTimeout));

    destroy_window(conn, w);
}

TEST_CASE("Integration: _NET_WM_STATE add below sets state atom", "[integration][wm_state][below]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t state_below = intern_atom(conn.get(), "_NET_WM_STATE_BELOW");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));
    REQUIRE_FALSE(has_state(conn, w, state_below));

    send_wm_state_change(conn, w, 1, state_below);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_below); }, kTimeout));

    destroy_window(conn, w);
}

TEST_CASE("Integration: above and below are mutually exclusive", "[integration][wm_state][above][below]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t state_above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");
    xcb_atom_t state_below = intern_atom(conn.get(), "_NET_WM_STATE_BELOW");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));

    // Set above first
    send_wm_state_change(conn, w, 1, state_above);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_above); }, kTimeout));

    // Setting below should clear above
    send_wm_state_change(conn, w, 1, state_below);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_below); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_above); }, kTimeout));

    // Setting above should clear below
    send_wm_state_change(conn, w, 1, state_above);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_above); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_below); }, kTimeout));

    destroy_window(conn, w);
}

TEST_CASE(
    "Integration: toggling above after below re-enables above",
    "[integration][wm_state][above][below]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t state_above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");
    xcb_atom_t state_below = intern_atom(conn.get(), "_NET_WM_STATE_BELOW");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));

    send_wm_state_change(conn, w, 1, state_above);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_above); }, kTimeout));

    send_wm_state_change(conn, w, 1, state_below);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_below); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_above); }, kTimeout));

    send_wm_state_change(conn, w, 2, state_above);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_above); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_below); }, kTimeout));

    destroy_window(conn, w);
}

TEST_CASE("Integration: initial above/below conflict is normalized", "[integration][wm_state][above][below]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t state_above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");
    xcb_atom_t state_below = intern_atom(conn.get(), "_NET_WM_STATE_BELOW");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    xcb_atom_t initial_states[] = { state_below, state_above };
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, w, net_wm_state, XCB_ATOM_ATOM, 32, 2, initial_states);
    xcb_flush(conn.get());

    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));

    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_above); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_below); }, kTimeout));

    send_wm_state_change(conn, w, 0, state_above);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_above); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_below); }, kTimeout));

    destroy_window(conn, w);
}

TEST_CASE("Integration: _NET_WM_STATE remove above clears state", "[integration][wm_state][above]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t state_above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));

    send_wm_state_change(conn, w, 1, state_above);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_above); }, kTimeout));

    send_wm_state_change(conn, w, 0, state_above);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_above); }, kTimeout));

    destroy_window(conn, w);
}

TEST_CASE("Integration: _NET_WM_STATE toggle cycles above", "[integration][wm_state][above]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t state_above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));

    // Toggle on
    send_wm_state_change(conn, w, 2, state_above);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_above); }, kTimeout));

    // Toggle off
    send_wm_state_change(conn, w, 2, state_above);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_above); }, kTimeout));

    destroy_window(conn, w);
}

// ─────────────────────────────────────────────────────────────────────────────
// Skip taskbar / Skip pager
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Integration: _NET_WM_STATE add skip_taskbar", "[integration][wm_state][skip_taskbar]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t skip_taskbar = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_TASKBAR");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));

    send_wm_state_change(conn, w, 1, skip_taskbar);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, skip_taskbar); }, kTimeout));

    send_wm_state_change(conn, w, 0, skip_taskbar);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, skip_taskbar); }, kTimeout));

    destroy_window(conn, w);
}

TEST_CASE("Integration: _NET_WM_STATE add skip_pager", "[integration][wm_state][skip_pager]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t skip_pager = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_PAGER");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));

    send_wm_state_change(conn, w, 1, skip_pager);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, skip_pager); }, kTimeout));

    send_wm_state_change(conn, w, 0, skip_pager);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, skip_pager); }, kTimeout));

    destroy_window(conn, w);
}

TEST_CASE("Integration: _NET_WM_STATE add/remove maximized atoms", "[integration][wm_state][maximized]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t maximized_horz = intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_HORZ");
    xcb_atom_t maximized_vert = intern_atom(conn.get(), "_NET_WM_STATE_MAXIMIZED_VERT");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));

    send_wm_state_change(conn, w, 1, maximized_horz, maximized_vert);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, maximized_horz); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, maximized_vert); }, kTimeout));

    send_wm_state_change(conn, w, 0, maximized_horz, maximized_vert);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, maximized_horz); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, maximized_vert); }, kTimeout));

    destroy_window(conn, w);
}

// ─────────────────────────────────────────────────────────────────────────────
// Demands attention
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Integration: _NET_WM_STATE add demands_attention", "[integration][wm_state][demands_attention]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t demands_attn = intern_atom(conn.get(), "_NET_WM_STATE_DEMANDS_ATTENTION");

    // Create two windows so w1 is not active (demands_attention is often cleared on active window)
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 30, 30, 200, 200);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    // Set demands_attention on the non-active window
    send_wm_state_change(conn, w1, 1, demands_attn);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w1, demands_attn); }, kTimeout));

    send_wm_state_change(conn, w1, 0, demands_attn);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w1, demands_attn); }, kTimeout));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Modal
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Integration: _NET_WM_STATE add modal sets above", "[integration][wm_state][modal]")
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t state_modal = intern_atom(conn.get(), "_NET_WM_STATE_MODAL");
    xcb_atom_t state_above = intern_atom(conn.get(), "_NET_WM_STATE_ABOVE");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));

    send_wm_state_change(conn, w, 1, state_modal);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_modal); }, kTimeout));
    // Modal windows should be raised above
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, state_above); }, kTimeout));

    send_wm_state_change(conn, w, 0, state_modal);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_modal); }, kTimeout));
    // Removing modal should clear above
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, state_above); }, kTimeout));

    destroy_window(conn, w);
}

// ─────────────────────────────────────────────────────────────────────────────
// Two-state message (first + second atom in one message)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Integration: _NET_WM_STATE handles two atoms in single message",
    "[integration][wm_state][multi]"
)
{
    auto test_env = TestEnvironment::create();
    if (!test_env)
        SKIP("Test environment not available");
    auto& conn = test_env->conn;

    xcb_atom_t skip_taskbar = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_TASKBAR");
    xcb_atom_t skip_pager = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_PAGER");

    xcb_window_t w = create_window(conn, 10, 10, 200, 200);
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, kTimeout));

    // Add both skip_taskbar and skip_pager in a single message
    send_wm_state_change(conn, w, 1, skip_taskbar, skip_pager);
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, skip_taskbar); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return has_state(conn, w, skip_pager); }, kTimeout));

    // Remove both
    send_wm_state_change(conn, w, 0, skip_taskbar, skip_pager);
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, skip_taskbar); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !has_state(conn, w, skip_pager); }, kTimeout));

    destroy_window(conn, w);
}
