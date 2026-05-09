#include "lwm/core/policy.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;
using namespace lwm::stacking_policy;

namespace {

ClientStackInputs make(
    xcb_window_t id,
    Tier tier = Tier::Normal,
    bool is_floating = false,
    bool is_active = false,
    uint64_t order = 0,
    bool visible = true)
{
    ClientStackInputs in;
    in.id = id;
    in.visible = visible;
    in.tier = tier;
    in.is_floating = is_floating;
    in.is_active = is_active;
    in.order = order;
    return in;
}

} // namespace

TEST_CASE("compute_tier respects layer precedence", "[stacking][policy]")
{
    REQUIRE(compute_tier(false, false, false, false, false, false) == Tier::Normal);

    // Below hint sinks to Below.
    REQUIRE(compute_tier(false, false, false, false, true, false) == Tier::Below);

    // Above hint or modal lifts to Above.
    REQUIRE(compute_tier(false, false, false, true, false, false) == Tier::Above);
    REQUIRE(compute_tier(false, false, false, false, false, true) == Tier::Above);

    // Fullscreen wins over Above/Below.
    REQUIRE(compute_tier(false, false, true, true, true, true) == Tier::Fullscreen);

    // A window suppressed by another's fullscreen sinks to Below regardless of its own hints.
    REQUIRE(compute_tier(false, true, false, true, false, true) == Tier::Below);

    // Overlay overrides everything else (including suppression and fullscreen).
    REQUIRE(compute_tier(true, true, true, true, true, true) == Tier::Overlay);
}

TEST_CASE("compute_order: floating ranks above tiled in the same tier", "[stacking][policy]")
{
    std::vector<ClientStackInputs> inputs = {
        make(0x100, Tier::Normal, /*floating=*/false, false, /*order=*/1),
        make(0x200, Tier::Normal, /*floating=*/true, false, /*order=*/2),
    };
    auto order = compute_order(inputs);
    REQUIRE(order.size() == 2);
    REQUIRE(order.front() == 0x100); // bottom
    REQUIRE(order.back() == 0x200);  // top
}

TEST_CASE("compute_order: ordering is global across monitors", "[stacking][policy]")
{
    // Reproduce the original bug: floating window on monitor A, recently raised
    // tile on monitor B.  Without a global ordering policy, the tile would
    // remain above the floating window in the X stack.
    std::vector<ClientStackInputs> inputs = {
        // Monitor A — tiled parent + floating dialog
        make(/*id=*/0x101, Tier::Normal, /*floating=*/false, false, /*order=*/1),
        make(/*id=*/0x102, Tier::Normal, /*floating=*/true,  false, /*order=*/2),
        // Monitor B — recently mapped tile (highest order) but should NOT be
        // globally above monitor A's floating dialog.
        make(/*id=*/0x201, Tier::Normal, /*floating=*/false, false, /*order=*/3),
    };

    auto order = compute_order(inputs);
    REQUIRE(order.size() == 3);

    // Tiled windows go to the bottom regardless of monitor; the floating dialog
    // ends up at the top.
    REQUIRE(order.back() == 0x102);

    auto rank = [&](xcb_window_t w) {
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == w) return i;
        return size_t{ static_cast<size_t>(-1) };
    };

    REQUIRE(rank(0x102) > rank(0x201));
    REQUIRE(rank(0x102) > rank(0x101));
}

TEST_CASE("compute_order: tiers strictly dominate kind/active/order", "[stacking][policy]")
{
    std::vector<ClientStackInputs> inputs = {
        // Active floating in Normal tier, but a Below-tier floating ranks below.
        make(0x10, Tier::Below,   true,  true,  100),
        // Floating Above-tier window
        make(0x20, Tier::Above,   true,  false, 1),
        // Fullscreen tile
        make(0x30, Tier::Fullscreen, false, false, 2),
        // Overlay
        make(0x40, Tier::Overlay, true,  false, 0),
        // Normal tile
        make(0x50, Tier::Normal,  false, false, 50),
    };

    auto order = compute_order(inputs);
    REQUIRE(order.size() == 5);

    // Bottom up: Below, Normal, Above, Fullscreen, Overlay.
    REQUIRE(order[0] == 0x10);
    REQUIRE(order[1] == 0x50);
    REQUIRE(order[2] == 0x20);
    REQUIRE(order[3] == 0x30);
    REQUIRE(order[4] == 0x40);
}

TEST_CASE("compute_order: hidden windows sink below visible ones", "[stacking][policy]")
{
    std::vector<ClientStackInputs> inputs = {
        make(0xA, Tier::Overlay, true,  false, 0, /*visible=*/false),
        make(0xB, Tier::Below,   false, false, 0, /*visible=*/true),
    };
    auto order = compute_order(inputs);
    REQUIRE(order.size() == 2);
    REQUIRE(order.front() == 0xA); // hidden, regardless of overlay tier
    REQUIRE(order.back() == 0xB);  // visible, regardless of below tier
}

TEST_CASE("compute_order: active window beats inactive within same tier and kind", "[stacking][policy]")
{
    std::vector<ClientStackInputs> inputs = {
        make(0x1, Tier::Normal, false, false, 10),
        make(0x2, Tier::Normal, false, true,  5),
    };
    auto order = compute_order(inputs);
    REQUIRE(order.back() == 0x2);
}

TEST_CASE("compute_order: order field is the ultimate tiebreaker", "[stacking][policy]")
{
    std::vector<ClientStackInputs> inputs = {
        make(0x1, Tier::Normal, true, false, 50),
        make(0x2, Tier::Normal, true, false, 60),
        make(0x3, Tier::Normal, true, false, 40),
    };
    auto order = compute_order(inputs);
    REQUIRE(order[0] == 0x3);
    REQUIRE(order[1] == 0x1);
    REQUIRE(order[2] == 0x2);
}
