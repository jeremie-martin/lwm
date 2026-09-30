#include "lwm/core/floating.hpp"
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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Client default state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Client has sensible defaults", "[client][state]")
{
    Client c;

    REQUIRE(c.id == XCB_NONE);
    REQUIRE(c.kind() == Client::Kind::Tiled);
    REQUIRE(c.monitor == 0);
    REQUIRE(c.workspace == 0);

    // All state flags should default to false
    REQUIRE_FALSE(c.hidden);
    REQUIRE_FALSE(c.fullscreen);
    REQUIRE(c.layer_hint == lwm::LayerHint::Normal);
    REQUIRE_FALSE(c.iconic);
    REQUIRE_FALSE(c.sticky);
    REQUIRE_FALSE(c.maximized_horz);
    REQUIRE_FALSE(c.maximized_vert);

    REQUIRE_FALSE(c.modal);
    REQUIRE_FALSE(c.skip_taskbar);
    REQUIRE_FALSE(c.skip_pager);
    REQUIRE_FALSE(c.app_prefs.skip_taskbar);
    REQUIRE_FALSE(c.app_prefs.skip_pager);
    REQUIRE_FALSE(c.app_prefs.above);
    REQUIRE_FALSE(c.app_prefs.below);
    REQUIRE_FALSE(c.urgency.active());
    REQUIRE_FALSE(c.urgency.has(UrgencySource::App));
    REQUIRE_FALSE(c.urgency.has(UrgencySource::WmInitiated));
    REQUIRE_FALSE(c.ignore_next_wm_hints_urgency_echo);
    REQUIRE_FALSE(c.borderless);
    REQUIRE_FALSE(c.desktop_pinned);

    // Restore geometries should be empty
    REQUIRE(tiled_state(c) != nullptr);
    REQUIRE_FALSE(prior_floating_geometry(c).has_value());
    REQUIRE_FALSE(c.fullscreen_monitors.has_value());
}

TEST_CASE("Urgency tracks app and WM sources independently", "[client][state][urgency]")
{
    Urgency urgency;

    REQUIRE_FALSE(urgency.active());
    REQUIRE(urgency.add(UrgencySource::App));
    REQUIRE_FALSE(urgency.add(UrgencySource::App));
    REQUIRE(urgency.active());
    REQUIRE(urgency.has(UrgencySource::App));
    REQUIRE_FALSE(urgency.has(UrgencySource::WmInitiated));

    REQUIRE(urgency.add(UrgencySource::WmInitiated));
    REQUIRE(urgency.remove(UrgencySource::App));
    REQUIRE(urgency.active());
    REQUIRE_FALSE(urgency.has(UrgencySource::App));
    REQUIRE(urgency.has(UrgencySource::WmInitiated));

    REQUIRE(urgency.clear());
    REQUIRE_FALSE(urgency.active());
}

TEST_CASE(
    "Fullscreen selection reads placement and honors preferred then current then newest owner",
    "[client][fullscreen]"
)
{
    std::unordered_map<xcb_window_t, Client> clients;
    std::vector<Monitor> monitors(2);
    for (auto& m : monitors) m.workspaces.resize(2);
    for (xcb_window_t id : { 1, 2, 3 })
    {
        Client client;
        client.id = id;
        client.order = id;
        client.fullscreen = true;
        clients.emplace(id, client);
    }
    monitors[0].fullscreen_owner = 1;
    auto select = [&](xcb_window_t preferred = XCB_NONE)
    { return fullscreen_policy::select_owner(clients, monitors, 0, false, preferred); };
    REQUIRE(select() == 1);
    REQUIRE(select(2) == 2);
    clients.at(2).iconic = true;
    REQUIRE(select(2) == 1);
    clients.at(1).workspace = 1;
    REQUIRE(select() == 3);
    clients.at(3).monitor = 1;
    REQUIRE(select() == XCB_NONE);
    clients.at(1).sticky = true;
    REQUIRE(select() == 1);
    clients.at(1).state = DockState{ };
    REQUIRE(select() == XCB_NONE);
    REQUIRE(fullscreen_policy::select_owner(clients, monitors, 1, true) == XCB_NONE);
    REQUIRE(fullscreen_policy::select_owner(clients, monitors, 2, false) == XCB_NONE);
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
// Above/Below state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("compute_desired_state enforces above/below mutual exclusion", "[client][state][stacking][policy]")
{
    SECTION("Above set clears below")
    {
        classification_policy::DesiredStateInputs in{};
        in.app_above = true;
        in.app_below = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.layer_hint == LayerHint::Above);
    }

    SECTION("Only below takes effect alone")
    {
        classification_policy::DesiredStateInputs in{};
        in.app_below = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.layer_hint == LayerHint::Below);
    }
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

    // Show desktop hides non-sticky windows
    REQUIRE_FALSE(visibility_policy::is_window_visible(true, false, false, 0, 0, monitors));
    // Sticky windows survive show-desktop
    REQUIRE(visibility_policy::is_window_visible(true, false, true, 0, 0, monitors));
    // Iconic sticky windows are still hidden during show-desktop
    REQUIRE_FALSE(visibility_policy::is_window_visible(true, true, true, 0, 0, monitors));

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

TEST_CASE("Modal suppresses explicit above in desired state", "[client][state][combination][policy]")
{
    SECTION("Modal suppresses EWMH above")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_modal = true;
        in.app_above = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.modal);
        REQUIRE(out.layer_hint == LayerHint::Normal);
    }

    SECTION("Modal suppresses classification above")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_modal = true;
        in.classification_above = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.modal);
        REQUIRE(out.layer_hint == LayerHint::Normal);
    }

    SECTION("Without modal, classification above works")
    {
        classification_policy::DesiredStateInputs in{};
        in.classification_above = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE_FALSE(out.modal);
        REQUIRE(out.layer_hint == LayerHint::Above);
    }

    SECTION("rule layer_hint overrides modal suppression")
    {
        classification_policy::DesiredStateInputs in{};
        in.ewmh_modal = true;
        in.rule_layer_hint = LayerHint::Above;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.modal);
        REQUIRE(out.layer_hint == LayerHint::Above);
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
    REQUIRE(out.layer_hint == LayerHint::Normal);
    REQUIRE_FALSE(out.borderless);
}

TEST_CASE("compute_desired_state: skip flags merge classification, app prefs, and transient", "[policy][classification]")
{
    SECTION("Classification skip_taskbar propagates")
    {
        classification_policy::DesiredStateInputs in{};
        in.classification_skip_taskbar = true;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.skip_taskbar);
    }

    SECTION("App skip_pager propagates")
    {
        classification_policy::DesiredStateInputs in{};
        in.app_skip_pager = true;
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
        in.app_skip_pager = true;
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

TEST_CASE("compute_desired_state: modal clears below", "[policy][classification]")
{
    classification_policy::DesiredStateInputs in{};
    in.ewmh_modal = true;
    in.app_below = true;
    auto out = classification_policy::compute_desired_state(in);

    REQUIRE(out.modal);
    REQUIRE(out.layer_hint == LayerHint::Normal);
}

TEST_CASE("compute_desired_state: rule layer_hint overrides ewmh below", "[policy][classification]")
{
    SECTION("Rule clears ewmh below")
    {
        classification_policy::DesiredStateInputs in{};
        in.app_below = true;
        in.rule_layer_hint = LayerHint::Normal;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.layer_hint == LayerHint::Normal);
    }

    SECTION("Rule forces below on")
    {
        classification_policy::DesiredStateInputs in{};
        in.rule_layer_hint = LayerHint::Below;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.layer_hint == LayerHint::Below);
    }
}

TEST_CASE("compute_desired_state: rule layer_hint overrides classification", "[policy][classification]")
{
    SECTION("Rule forces above off despite classification")
    {
        classification_policy::DesiredStateInputs in{};
        in.classification_above = true;
        in.rule_layer_hint = LayerHint::Normal;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.layer_hint == LayerHint::Normal);
    }

    SECTION("Rule forces above on without classification")
    {
        classification_policy::DesiredStateInputs in{};
        in.rule_layer_hint = LayerHint::Above;
        auto out = classification_policy::compute_desired_state(in);
        REQUIRE(out.layer_hint == LayerHint::Above);
    }
}

TEST_CASE("compute_desired_state: borderless follows the window rule", "[policy][classification]")
{
    classification_policy::DesiredStateInputs in{};
    in.rule_borderless = true;
    auto out = classification_policy::compute_desired_state(in);
    REQUIRE(out.borderless);
}

TEST_CASE("Maximize presentation preserves normal placement", "[client][state][floating]")
{
    Geometry normal{ 100, 120, 500, 360 }, area{ 0, 0, 1920, 1080 };
    CHECK(floating::presentation_geometry(normal, area, false, false) == normal);
    CHECK(floating::presentation_geometry(normal, area, true, false) == Geometry{ 0, 120, 1920, 360 });
    CHECK(floating::presentation_geometry(normal, area, false, true) == Geometry{ 100, 0, 500, 1080 });
    CHECK(floating::presentation_geometry(normal, area, true, true) == area);
    CHECK(normal == Geometry{ 100, 120, 500, 360 });
}
