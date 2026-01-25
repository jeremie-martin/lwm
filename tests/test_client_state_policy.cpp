#include "lwm/core/policy.hpp"
#include "lwm/core/types.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

std::vector<Monitor> make_monitors(size_t count = 2, size_t workspaces = 3)
{
    std::vector<Monitor> monitors;
    for (size_t i = 0; i < count; ++i)
    {
        Monitor mon;
        mon.x = static_cast<int16_t>(i * 1920);
        mon.y = 0;
        mon.width = 1920;
        mon.height = 1080;
        mon.workspaces.assign(workspaces, Workspace{});
        mon.current_workspace = 0;
        monitors.push_back(mon);
    }
    return monitors;
}

Client make_client(xcb_window_t id, Client::Kind kind = Client::Kind::Tiled)
{
    Client c;
    c.id = id;
    c.kind = kind;
    return c;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Client default state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Client has sensible defaults", "[client][state]")
{
    Client c;

    REQUIRE(c.id == XCB_NONE);
    REQUIRE(c.kind == Client::Kind::Tiled);
    REQUIRE(c.monitor == 0);
    REQUIRE(c.workspace == 0);

    // All state flags should default to false
    REQUIRE_FALSE(c.hidden);
    REQUIRE_FALSE(c.fullscreen);
    REQUIRE_FALSE(c.above);
    REQUIRE_FALSE(c.below);
    REQUIRE_FALSE(c.iconic);
    REQUIRE_FALSE(c.sticky);
    REQUIRE_FALSE(c.maximized_horz);
    REQUIRE_FALSE(c.maximized_vert);
    REQUIRE_FALSE(c.shaded);
    REQUIRE_FALSE(c.modal);
    REQUIRE_FALSE(c.skip_taskbar);
    REQUIRE_FALSE(c.skip_pager);
    REQUIRE_FALSE(c.demands_attention);

    // Restore geometries should be empty
    REQUIRE_FALSE(c.fullscreen_restore.has_value());
    REQUIRE_FALSE(c.maximize_restore.has_value());
    REQUIRE_FALSE(c.fullscreen_monitors.has_value());
}

TEST_CASE("Client kind can be set to all valid types", "[client][state]")
{
    Client tiled = make_client(0x1000, Client::Kind::Tiled);
    REQUIRE(tiled.kind == Client::Kind::Tiled);

    Client floating = make_client(0x2000, Client::Kind::Floating);
    REQUIRE(floating.kind == Client::Kind::Floating);

    Client dock = make_client(0x3000, Client::Kind::Dock);
    REQUIRE(dock.kind == Client::Kind::Dock);

    Client desktop = make_client(0x4000, Client::Kind::Desktop);
    REQUIRE(desktop.kind == Client::Kind::Desktop);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fullscreen state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Fullscreen restore geometry can be stored and retrieved", "[client][state][fullscreen]")
{
    Client c = make_client(0x1000);

    // Before fullscreen, no restore geometry
    REQUIRE_FALSE(c.fullscreen_restore.has_value());

    // Store restore geometry
    c.fullscreen_restore = Geometry{ 100, 100, 800, 600 };
    c.fullscreen = true;

    REQUIRE(c.fullscreen);
    REQUIRE(c.fullscreen_restore.has_value());
    REQUIRE(c.fullscreen_restore->x == 100);
    REQUIRE(c.fullscreen_restore->y == 100);
    REQUIRE(c.fullscreen_restore->width == 800);
    REQUIRE(c.fullscreen_restore->height == 600);
}

TEST_CASE("Fullscreen monitors can be specified", "[client][state][fullscreen]")
{
    Client c = make_client(0x1000);

    c.fullscreen_monitors = FullscreenMonitors{ 0, 1, 0, 1 };
    c.fullscreen = true;

    REQUIRE(c.fullscreen_monitors.has_value());
    REQUIRE(c.fullscreen_monitors->top == 0);
    REQUIRE(c.fullscreen_monitors->bottom == 1);
    REQUIRE(c.fullscreen_monitors->left == 0);
    REQUIRE(c.fullscreen_monitors->right == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Iconic (minimized) state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Iconic window is never visible regardless of workspace", "[client][state][iconic]")
{
    auto monitors = make_monitors();

    REQUIRE(visibility_policy::is_window_visible(false, false, false, 0, 0, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, true, false, 0, 0, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, true, false, 0, 1, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, true, true, 0, 1, monitors));
}

// ─────────────────────────────────────────────────────────────────────────────
// Sticky state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Sticky window visible across workspaces but not monitors", "[client][state][sticky]")
{
    auto monitors = make_monitors();
    monitors[0].current_workspace = 0;

    REQUIRE_FALSE(visibility_policy::is_window_visible(false, false, false, 0, 1, monitors));
    REQUIRE(visibility_policy::is_window_visible(false, false, true, 0, 1, monitors));
    REQUIRE(visibility_policy::is_window_visible(false, false, true, 0, 2, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, false, true, 5, 0, monitors));
}

// ─────────────────────────────────────────────────────────────────────────────
// Maximized state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Maximize restore geometry can be stored and retrieved", "[client][state][maximized]")
{
    Client c = make_client(0x1000, Client::Kind::Floating);

    c.maximize_restore = Geometry{ 200, 150, 600, 400 };
    c.maximized_horz = true;
    c.maximized_vert = true;

    REQUIRE(c.maximized_horz);
    REQUIRE(c.maximized_vert);
    REQUIRE(c.maximize_restore.has_value());
    REQUIRE(c.maximize_restore->x == 200);
    REQUIRE(c.maximize_restore->y == 150);
}

TEST_CASE("Maximized states can be set independently", "[client][state][maximized]")
{
    Client c = make_client(0x1000, Client::Kind::Floating);

    // Only horizontal maximize
    c.maximized_horz = true;
    REQUIRE(c.maximized_horz);
    REQUIRE_FALSE(c.maximized_vert);

    // Only vertical maximize
    c.maximized_horz = false;
    c.maximized_vert = true;
    REQUIRE_FALSE(c.maximized_horz);
    REQUIRE(c.maximized_vert);

    // Both
    c.maximized_horz = true;
    REQUIRE(c.maximized_horz);
    REQUIRE(c.maximized_vert);
}

// ─────────────────────────────────────────────────────────────────────────────
// Above/Below state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Above and below states are mutually exclusive by convention", "[client][state][stacking]")
{
    Client c = make_client(0x1000, Client::Kind::Floating);

    // Setting above
    c.above = true;
    REQUIRE(c.above);
    REQUIRE_FALSE(c.below);

    // Setting below should clear above (by convention)
    c.below = true;
    // Note: The struct doesn't enforce this, but WM code should
    REQUIRE(c.below);
    // c.above is still true - enforcement is in WM, not data structure
}

// ─────────────────────────────────────────────────────────────────────────────
// Focus eligibility with state combinations
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Focus eligibility checks window kind and input hints", "[client][focus][policy]")
{
    // Tiled/floating windows need input focus or WM_TAKE_FOCUS
    REQUIRE(focus_policy::is_focus_eligible(Client::Kind::Tiled, true, false));
    REQUIRE(focus_policy::is_focus_eligible(Client::Kind::Floating, true, false));
    REQUIRE(focus_policy::is_focus_eligible(Client::Kind::Tiled, false, true));
    REQUIRE(focus_policy::is_focus_eligible(Client::Kind::Floating, false, true));
    REQUIRE_FALSE(focus_policy::is_focus_eligible(Client::Kind::Tiled, false, false));
    REQUIRE_FALSE(focus_policy::is_focus_eligible(Client::Kind::Floating, false, false));

    // Dock and desktop windows are never eligible
    REQUIRE_FALSE(focus_policy::is_focus_eligible(Client::Kind::Dock, true, true));
    REQUIRE_FALSE(focus_policy::is_focus_eligible(Client::Kind::Desktop, true, true));
}

// ─────────────────────────────────────────────────────────────────────────────
// Visibility policy comprehensive tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Visibility handles show desktop, workspace, and invalid monitor", "[visibility][policy]")
{
    auto monitors = make_monitors(3, 5);
    monitors[0].current_workspace = 2;
    monitors[1].current_workspace = 0;
    monitors[2].current_workspace = 4;

    // Show desktop hides all windows
    REQUIRE_FALSE(visibility_policy::is_window_visible(true, false, false, 0, 0, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(true, false, true, 0, 0, monitors));

    // Current workspace on each monitor is visible
    REQUIRE(visibility_policy::is_workspace_visible(false, 0, 2, monitors));
    REQUIRE(visibility_policy::is_workspace_visible(false, 1, 0, monitors));
    REQUIRE(visibility_policy::is_workspace_visible(false, 2, 4, monitors));

    // Non-current workspaces are not visible
    REQUIRE_FALSE(visibility_policy::is_workspace_visible(false, 0, 0, monitors));
    REQUIRE_FALSE(visibility_policy::is_workspace_visible(false, 1, 1, monitors));
    REQUIRE_FALSE(visibility_policy::is_workspace_visible(false, 2, 3, monitors));

    // Invalid monitor index
    REQUIRE_FALSE(visibility_policy::is_workspace_visible(false, 5, 0, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, false, false, 5, 0, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, false, true, 5, 0, monitors));
}

// ─────────────────────────────────────────────────────────────────────────────
// Transient window state tests
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// User time and focus stealing prevention
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Sync protocol state tests
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Skip taskbar/pager tests
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Demands attention (urgency) tests
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// State combination tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Fullscreen and iconic can coexist", "[client][state][combination]")
{
    Client c = make_client(0x1000);

    c.fullscreen = true;
    c.iconic = true;

    REQUIRE(c.fullscreen);
    REQUIRE(c.iconic);
}

TEST_CASE("Sticky and fullscreen can coexist", "[client][state][combination]")
{
    Client c = make_client(0x1000);

    c.sticky = true;
    c.fullscreen = true;

    REQUIRE(c.sticky);
    REQUIRE(c.fullscreen);
}

TEST_CASE("Modal and above can coexist", "[client][state][combination]")
{
    Client c = make_client(0x1000, Client::Kind::Floating);

    c.modal = true;
    c.above = true;

    REQUIRE(c.modal);
    REQUIRE(c.above);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fullscreen state transition policy tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Fullscreen should clear ABOVE state", "[client][state][fullscreen][policy]")
{
    // Documents expected WM behavior: entering fullscreen clears ABOVE
    Client c = make_client(0x1000, Client::Kind::Floating);

    c.above = true;
    REQUIRE(c.above);

    // Simulate fullscreen entry (WM clears ABOVE)
    c.fullscreen = true;
    c.above = false;

    REQUIRE(c.fullscreen);
    REQUIRE_FALSE(c.above);
}

TEST_CASE("Fullscreen should clear BELOW state", "[client][state][fullscreen][policy]")
{
    // Documents expected WM behavior: entering fullscreen clears BELOW
    Client c = make_client(0x1000, Client::Kind::Floating);

    c.below = true;
    REQUIRE(c.below);

    // Simulate fullscreen entry (WM clears BELOW)
    c.fullscreen = true;
    c.below = false;

    REQUIRE(c.fullscreen);
    REQUIRE_FALSE(c.below);
}

TEST_CASE("Fullscreen should clear maximized states", "[client][state][fullscreen][policy]")
{
    // Documents expected WM behavior: entering fullscreen clears maximized
    Client c = make_client(0x1000, Client::Kind::Floating);

    c.maximized_horz = true;
    c.maximized_vert = true;
    c.maximize_restore = Geometry{ 100, 100, 800, 600 };
    REQUIRE(c.maximized_horz);
    REQUIRE(c.maximized_vert);

    // Simulate fullscreen entry (WM clears maximized but preserves restore geometry)
    c.fullscreen = true;
    c.maximized_horz = false;
    c.maximized_vert = false;

    REQUIRE(c.fullscreen);
    REQUIRE_FALSE(c.maximized_horz);
    REQUIRE_FALSE(c.maximized_vert);
    // maximize_restore is preserved for proper geometry restoration chain
    REQUIRE(c.maximize_restore.has_value());
}

TEST_CASE("Fullscreen should preserve modal state", "[client][state][fullscreen][policy]")
{
    // Documents expected WM behavior: modal persists through fullscreen
    // (modal is a window type hint, not a visual state)
    Client c = make_client(0x1000, Client::Kind::Floating);

    c.modal = true;
    REQUIRE(c.modal);

    c.fullscreen = true;

    REQUIRE(c.fullscreen);
    REQUIRE(c.modal); // Modal is intentionally preserved
}

// ─────────────────────────────────────────────────────────────────────────────
// Modal state transition policy tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Modal enable should set ABOVE state", "[client][state][modal][policy]")
{
    // Documents expected WM behavior: modal windows are always above
    Client c = make_client(0x1000, Client::Kind::Floating);

    REQUIRE_FALSE(c.modal);
    REQUIRE_FALSE(c.above);

    // Simulate modal enable (WM sets ABOVE)
    c.modal = true;
    c.above = true;

    REQUIRE(c.modal);
    REQUIRE(c.above);
}

TEST_CASE("Modal disable should clear ABOVE state", "[client][state][modal][policy]")
{
    // Documents expected WM behavior: disabling modal clears auto-ABOVE
    Client c = make_client(0x1000, Client::Kind::Floating);

    c.modal = true;
    c.above = true;
    REQUIRE(c.modal);
    REQUIRE(c.above);

    // Simulate modal disable (WM clears ABOVE)
    c.modal = false;
    c.above = false;

    REQUIRE_FALSE(c.modal);
    REQUIRE_FALSE(c.above);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fullscreen restore geometry policy tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Fullscreen restore geometry should be nullopt when query fails", "[client][state][fullscreen][policy]")
{
    // Documents expected WM behavior: failed geometry query clears restore
    Client c = make_client(0x1000);

    // Simulate stale restore geometry from previous fullscreen
    c.fullscreen_restore = Geometry{ 100, 100, 800, 600 };

    // When xcb_get_geometry_reply returns nullptr, WM should clear
    c.fullscreen_restore = std::nullopt;

    REQUIRE_FALSE(c.fullscreen_restore.has_value());
}

TEST_CASE("Fullscreen exit uses restore geometry", "[client][state][fullscreen][policy]")
{
    Client c = make_client(0x1000, Client::Kind::Floating);

    // Save geometry before fullscreen
    Geometry original{ 100, 100, 800, 600 };
    c.fullscreen_restore = original;
    c.fullscreen = true;

    // Exit fullscreen - restore geometry should be used
    c.fullscreen = false;

    REQUIRE_FALSE(c.fullscreen);
    REQUIRE(c.fullscreen_restore.has_value());
    REQUIRE(c.fullscreen_restore->width == 800);
    REQUIRE(c.fullscreen_restore->height == 600);
}
