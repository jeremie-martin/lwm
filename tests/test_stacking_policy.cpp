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
    REQUIRE(compute_tier(false, false, false, false, false) == Tier::Normal);

    // Below hint sinks to Below.
    REQUIRE(compute_tier(false, false, false, true, false) == Tier::Below);

    // Above hint or modal lifts to Above.
    REQUIRE(compute_tier(false, false, true, false, false) == Tier::Above);
    REQUIRE(compute_tier(false, false, false, false, true) == Tier::Above);

    // Fullscreen wins over Above/Below.
    REQUIRE(compute_tier(false, true, true, true, true) == Tier::Fullscreen);

    // A window suppressed by another's fullscreen sinks to Below regardless of its own hints.
    REQUIRE(compute_tier(true, false, true, false, true) == Tier::Below);
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
        // Normal tile
        make(0x50, Tier::Normal,  false, false, 50),
    };

    auto order = compute_order(inputs);
    REQUIRE(order.size() == 4);

    // Bottom up: Below, Normal, Above, Fullscreen.
    REQUIRE(order[0] == 0x10);
    REQUIRE(order[1] == 0x50);
    REQUIRE(order[2] == 0x20);
    REQUIRE(order[3] == 0x30);
}

TEST_CASE("compute_order: hidden windows sink below visible ones", "[stacking][policy]")
{
    std::vector<ClientStackInputs> inputs = {
        make(0xA, Tier::Fullscreen, true,  false, 0, /*visible=*/false),
        make(0xB, Tier::Below,   false, false, 0, /*visible=*/true),
    };
    auto order = compute_order(inputs);
    REQUIRE(order.size() == 2);
    REQUIRE(order.front() == 0xA); // hidden, regardless of fullscreen tier
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

TEST_CASE("Stack moves produce the requested order with the minimum number of moves", "[stacking][policy]")
{
    for (size_t n = 0; n <= 6; ++n)
    {
        std::vector<xcb_window_t> desired;
        for (size_t i = 0; i < n; ++i) desired.push_back(static_cast<xcb_window_t>(i + 1));
        auto actual = desired;
        do {
            // Independent exhaustive reference: largest already-ordered subset.
            size_t longest = 0;
            for (size_t mask = 0; mask < (size_t{ 1 } << n); ++mask)
            {
                std::vector<xcb_window_t> subset;
                for (size_t i = 0; i < n; ++i)
                    if (mask & (size_t{ 1 } << i))
                        subset.push_back(actual[i]);
                if (std::is_sorted(subset.begin(), subset.end()))
                    longest = std::max(longest, subset.size());
            }
            auto moves = plan_moves(actual, desired);
            REQUIRE(moves.size() == n - longest);
            auto result = actual;
            for (auto move : moves)
            {
                std::erase(result, move.window);
                auto sibling = std::ranges::find(result, move.sibling);
                REQUIRE(sibling != result.end());
                result.insert(sibling + (move.mode == XCB_STACK_MODE_ABOVE ? 1 : 0), move.window);
            }
            REQUIRE(result == desired);
        } while (std::next_permutation(actual.begin(), actual.end()));
    }
}

TEST_CASE("Stack planning ignores unrelated and already-destroyed root children", "[stacking][policy]")
{
    std::vector<xcb_window_t> actual{ 90, 3, 91, 1, 92, 2 };
    std::vector<xcb_window_t> desired{ 1, 2, 3, 4 };
    auto moves = plan_moves(actual, desired);
    REQUIRE(moves.size() == 1);
    CHECK(moves[0].window == 3);
    CHECK(moves[0].sibling == 2);
    CHECK(moves[0].mode == XCB_STACK_MODE_ABOVE);
}
