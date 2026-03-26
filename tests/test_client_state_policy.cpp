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

    REQUIRE_FALSE(c.modal);
    REQUIRE_FALSE(c.skip_taskbar);
    REQUIRE_FALSE(c.skip_pager);
    REQUIRE_FALSE(c.demands_attention);
    REQUIRE_FALSE(c.borderless);
    REQUIRE(c.layer == WindowLayer::Normal);

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

TEST_CASE("compute_desired_state enforces above/below mutual exclusion", "[client][state][stacking][policy]")
{
    SECTION("Above set clears below")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_above = true;
        in.ewmh_below = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.above);
        REQUIRE_FALSE(out.below);
    }

    SECTION("Only below takes effect alone")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_below = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE_FALSE(out.above);
        REQUIRE(out.below);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Focus eligibility with state combinations
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Focus eligibility checks input hints", "[client][focus][policy]")
{
    // Windows need input focus or WM_TAKE_FOCUS
    REQUIRE(focus_policy::is_focus_eligible(true, false));
    REQUIRE(focus_policy::is_focus_eligible(false, true));
    REQUIRE(focus_policy::is_focus_eligible(true, true));
    REQUIRE_FALSE(focus_policy::is_focus_eligible(false, false));
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

TEST_CASE("Modal suppresses explicit above in desired state", "[client][state][combination][policy]")
{
    SECTION("Modal suppresses EWMH above")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_modal = true;
        in.ewmh_above = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.modal);
        REQUIRE_FALSE(out.above);
    }

    SECTION("Modal suppresses classification above")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_modal = true;
        in.classification_above = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.modal);
        REQUIRE_FALSE(out.above);
    }

    SECTION("Without modal, classification above works")
    {
        classification_policy::DesiredStateInputs in{};
        in.classification_above = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE_FALSE(out.modal);
        REQUIRE(out.above);
    }

    SECTION("rule_above overrides modal suppression")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_modal = true;
        in.rule_above = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.modal);
        REQUIRE(out.above);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// compute_desired_state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("compute_desired_state defaults are all false", "[policy][classification]")
{
    classification_policy::DesiredStateInputs in{};
    auto out = classification_policy::compute_desired_state(in);

    REQUIRE_FALSE(out.skip_taskbar);
    REQUIRE_FALSE(out.skip_pager);
    REQUIRE_FALSE(out.sticky);
    REQUIRE_FALSE(out.modal);
    REQUIRE_FALSE(out.above);
    REQUIRE_FALSE(out.below);
    REQUIRE_FALSE(out.borderless);
}

TEST_CASE("compute_desired_state: skip flags merge classification, ewmh, and transient", "[policy][classification]")
{
    SECTION("Classification skip_taskbar propagates")
    {
        classification_policy::DesiredStateInputs in{};
        in.classification_skip_taskbar = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.skip_taskbar);
    }

    SECTION("EWMH skip_pager propagates")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_skip_pager = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.skip_pager);
    }

    SECTION("Transient windows get skip flags")
    {
        classification_policy::DesiredStateInputs in{};
        in.has_transient = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.skip_taskbar);
        REQUIRE(out.skip_pager);
    }

    SECTION("Rule overrides classification and EWMH")
    {
        classification_policy::DesiredStateInputs in{};
        in.classification_skip_taskbar = true;
        in.ewmh_skip_pager = true;
        in.rule_skip_taskbar = false;
        in.rule_skip_pager = false;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE_FALSE(out.skip_taskbar);
        REQUIRE_FALSE(out.skip_pager);
    }
}

TEST_CASE("compute_desired_state: sticky merges desktop flag, ewmh, and rules", "[policy][classification]")
{
    SECTION("EWMH sticky")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_sticky = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.sticky);
    }

    SECTION("Sticky desktop flag")
    {
        classification_policy::DesiredStateInputs in{};
        in.is_sticky_desktop = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.sticky);
    }

    SECTION("Rule overrides ewmh sticky off")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_sticky = true;
        in.rule_sticky = false;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE_FALSE(out.sticky);
    }

    SECTION("Rule forces sticky on")
    {
        classification_policy::DesiredStateInputs in{};
        in.rule_sticky = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.sticky);
    }
}

TEST_CASE("compute_desired_state: overlay layer forces skip, sticky, borderless, clears above/below", "[policy][classification]")
{
    classification_policy::DesiredStateInputs in{};
    in.layer = WindowLayer::Overlay;
    in.ewmh_above = true;
    in.ewmh_below = true;
    auto out = classification_policy::compute_desired_state(in);

    REQUIRE(out.skip_taskbar);
    REQUIRE(out.skip_pager);
    REQUIRE(out.sticky);
    REQUIRE(out.borderless);
    REQUIRE_FALSE(out.above);
    REQUIRE_FALSE(out.below);
}

TEST_CASE("compute_desired_state: modal clears below", "[policy][classification]")
{
    classification_policy::DesiredStateInputs in{};
    in.ewmh_modal = true;
    in.ewmh_below = true;
    auto out = classification_policy::compute_desired_state(in);

    REQUIRE(out.modal);
    REQUIRE_FALSE(out.below);
}

TEST_CASE("compute_desired_state: rule_below overrides ewmh", "[policy][classification]")
{
    SECTION("Rule suppresses ewmh below")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_below = true;
        in.rule_below = false;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE_FALSE(out.below);
    }

    SECTION("Rule forces below on")
    {
        classification_policy::DesiredStateInputs in{};
        in.rule_below = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.below);
    }
}

TEST_CASE("compute_desired_state: rule_above overrides classification", "[policy][classification]")
{
    SECTION("Rule forces above off despite classification")
    {
        classification_policy::DesiredStateInputs in{};
        in.classification_above = true;
        in.rule_above = false;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE_FALSE(out.above);
    }

    SECTION("Rule forces above on without classification")
    {
        classification_policy::DesiredStateInputs in{};
        in.rule_above = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.above);
    }
}

TEST_CASE("compute_desired_state: borderless only from rule or overlay", "[policy][classification]")
{
    SECTION("Rule sets borderless")
    {
        classification_policy::DesiredStateInputs in{};
        in.rule_borderless = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.borderless);
    }

    SECTION("Overlay forces borderless regardless of rule")
    {
        classification_policy::DesiredStateInputs in{};
        in.layer = WindowLayer::Overlay;
        in.rule_borderless = false;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.borderless);
    }
}
