#include "lwm/core/stacking.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;
using namespace lwm::stacking;
namespace {
struct Scene
{
    std::unordered_map<xcb_window_t, Client> clients;
    std::vector<Monitor> monitors{ 2 };
    Scene()
    {
        for (auto& m : monitors) m.workspaces.resize(2);
    }
    Client& add(xcb_window_t id, bool floating = false, size_t monitor = 0)
    {
        Client c;
        c.id = id;
        c.order = id;
        c.monitor = monitor;
        if (floating)
            c.state = FloatingState{ };
        return clients.emplace(id, c).first->second;
    }
    std::vector<xcb_window_t> order(xcb_window_t active = XCB_NONE, bool desktop = false) const
    {
        return stacking::compute_order(clients, monitors, desktop, active);
    }
};
}

TEST_CASE("Global stacking ranks actual kinds, focus, layers and fullscreen visibility", "[stacking][policy]")
{
    Scene scene;
    scene.add(1);
    scene.add(2, false, 1);
    scene.add(3, true);
    scene.add(4, true, 1);
    REQUIRE(scene.order(1) == std::vector<xcb_window_t>{ 2, 1, 3, 4 });
    REQUIRE(scene.order(3) == std::vector<xcb_window_t>{ 1, 2, 4, 3 });
    scene.clients.at(3).layer_hint = LayerHint::Below;
    scene.clients.at(2).layer_hint = LayerHint::Above;
    REQUIRE(scene.order() == std::vector<xcb_window_t>{ 3, 1, 4, 2 });
    scene.add(5).state = DockState{ };
    scene.add(6).state = DesktopState{ };
    REQUIRE(scene.order() == std::vector<xcb_window_t>{ 6, 3, 1, 4, 2, 5 });
    scene.clients.at(2).fullscreen = true;
    scene.monitors[1].fullscreen_owner = 2;
    auto order = scene.order();
    REQUIRE(order == std::vector<xcb_window_t>{ 4, 6, 3, 1, 5, 2 });
    scene.clients.at(4).transient_for = 2;
    order = scene.order();
    REQUIRE(order == std::vector<xcb_window_t>{ 6, 3, 1, 5, 2, 4 });
}

TEST_CASE("Hidden clients precede visible clients and do not constrain visible transients", "[stacking][policy]")
{
    Scene scene;
    scene.add(1, true).transient_for = 2;
    scene.add(2).fullscreen = true;
    scene.add(3, true).modal = true;
    SECTION("Off workspace") { scene.clients.at(2).workspace = 1; }
    SECTION("Iconic") { scene.clients.at(2).iconic = true; }
    auto order = scene.order();
    REQUIRE(order == std::vector<xcb_window_t>{ 2, 1, 3 });
    scene.clients.at(1).sticky = true;
    order = scene.order(XCB_NONE, true);
    REQUIRE(order.back() == 1);
}

TEST_CASE(
    "Transient ordering matches exhaustive stable constraint ordering including every four-client cycle",
    "[stacking][policy]"
)
{
    // Enumerate every functional graph, including missing parents, self-links,
    // cycles with descendants, disjoint cycles and acyclic chains. An independent
    // reachability matrix identifies cycles; exhaustive permutations define the
    // lowest base-priority order satisfying all retained parent constraints.
    for (unsigned graph = 0; graph < 625; ++graph)
    {
        Scene scene;
        unsigned encoded = graph;
        bool reach[4][4]{ };
        unsigned parents[4]{ };
        for (unsigned i = 0; i < 4; ++i)
        {
            parents[i] = encoded % 5;
            encoded /= 5;
            scene.add(i + 1, true).transient_for = parents[i] ? parents[i] : 999;
            if (parents[i])
                reach[i][parents[i] - 1] = true;
        }
        for (unsigned k = 0; k < 4; ++k)
            for (unsigned i = 0; i < 4; ++i)
                for (unsigned j = 0; j < 4; ++j) reach[i][j] |= reach[i][k] && reach[k][j];
        for (unsigned i = 0; i < 4; ++i)
        {
            bool first = reach[i][i];
            for (unsigned j = 0; j < i; ++j) first &= !(reach[i][j] && reach[j][i]);
            if (first)
                parents[i] = 0;
        }
        std::vector<xcb_window_t> expected{ 1, 2, 3, 4 };
        bool valid;
        do
        {
            valid = true;
            for (unsigned i = 0; i < 4; ++i)
                if (parents[i])
                    valid &= std::ranges::find(expected, parents[i]) < std::ranges::find(expected, i + 1);
            if (valid)
                break;
        } while (std::next_permutation(expected.begin(), expected.end()));
        REQUIRE(valid);
        INFO("parent graph " << graph);
        auto order = scene.order();
        REQUIRE(order == expected);
    }
}

TEST_CASE("Long transient chains use bounded iterative ordering", "[stacking][policy]")
{
    Scene scene;
    for (xcb_window_t i = 1; i <= 10000; ++i) scene.add(i, true).transient_for = i + 1;
    auto order = scene.order();
    REQUIRE(order.size() == 10000);
    for (size_t i = 0; i < order.size(); ++i) REQUIRE(order[i] == 10000 - i);
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
