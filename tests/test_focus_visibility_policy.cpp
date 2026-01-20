#include <catch2/catch_test_macros.hpp>
#include "lwm/core/policy.hpp"

using namespace lwm;

namespace {

std::vector<Monitor> make_monitors()
{
    Monitor first;
    first.workspaces.assign(3, Workspace{});
    first.current_workspace = 1;

    Monitor second;
    second.workspaces.assign(2, Workspace{});
    second.current_workspace = 0;

    return { first, second };
}

} // namespace

TEST_CASE("Focus eligibility blocks dock and desktop", "[focus][policy]")
{
    REQUIRE_FALSE(focus_policy::is_focus_eligible(Client::Kind::Dock, true, true));
    REQUIRE_FALSE(focus_policy::is_focus_eligible(Client::Kind::Desktop, true, false));
}

TEST_CASE("Focus eligibility honors input or take focus", "[focus][policy]")
{
    REQUIRE(focus_policy::is_focus_eligible(Client::Kind::Tiled, true, false));
    REQUIRE(focus_policy::is_focus_eligible(Client::Kind::Floating, false, true));
    REQUIRE_FALSE(focus_policy::is_focus_eligible(Client::Kind::Tiled, false, false));
}

TEST_CASE("Workspace visibility respects showing desktop", "[visibility][policy]")
{
    auto monitors = make_monitors();

    REQUIRE_FALSE(visibility_policy::is_workspace_visible(true, 0, 1, monitors));
    REQUIRE_FALSE(visibility_policy::is_workspace_visible(false, 5, 0, monitors));
    REQUIRE(visibility_policy::is_workspace_visible(false, 0, 1, monitors));
    REQUIRE_FALSE(visibility_policy::is_workspace_visible(false, 0, 2, monitors));
}

TEST_CASE("Window visibility checks sticky and workspace match", "[visibility][policy]")
{
    auto monitors = make_monitors();

    REQUIRE_FALSE(visibility_policy::is_window_visible(true, false, false, 0, 1, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, true, false, 0, 1, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, false, false, 5, 0, monitors));

    REQUIRE(visibility_policy::is_window_visible(false, false, false, 0, 1, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, false, false, 0, 2, monitors));

    REQUIRE(visibility_policy::is_window_visible(false, false, true, 1, 1, monitors));
}
