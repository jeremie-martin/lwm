#include "lwm/layout/layout.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace lwm;

namespace {
struct LayoutFixture
{
    uint32_t padding = 10;
    LayoutConfig config;
    Layout layout() const { return Layout{ padding, config }; }
};
Workspace tiling(LayoutStrategy strategy, SplitRatioMap ratios = { })
{
    return { .layout_strategy = strategy, .split_ratios = std::move(ratios) };
}
}

TEST_CASE("Layout handles empty, single and monocle workspaces", "[layout]")
{
    LayoutFixture f;
    Geometry area{ -100, 20, 1000, 500 };
    Geometry content{ -90, 30, 980, 480 };
    CHECK(f.layout().arrange(0, area, tiling(LayoutStrategy::MasterStack)).empty());
    CHECK(f.layout().arrange(1, area, tiling(LayoutStrategy::MasterStack)) == std::vector<Geometry>{ content });
    CHECK(f.layout().arrange(4, area, tiling(LayoutStrategy::Monocle)) == std::vector<Geometry>(4, content));
    CHECK_FALSE(f.layout().hit_test(1, area, tiling(LayoutStrategy::MasterStack), 400, 250));
    CHECK_FALSE(f.layout().hit_test(4, area, tiling(LayoutStrategy::Monocle), 400, 250));
    CHECK(f.layout().drop_target_index(4, area, tiling(LayoutStrategy::Monocle), 400, 250) == 0);
}

TEST_CASE("Master-stack preserves pixel rounding and resize boundaries", "[layout]")
{
    LayoutFixture f;
    Geometry area{ 0, 0, 1000, 1000 };
    auto slots = f.layout().arrange(4, area, tiling(LayoutStrategy::MasterStack));
    REQUIRE(slots == std::vector<Geometry>{
        {10, 10, 485, 980}, {505, 10, 485, 323},
        {505, 343, 485, 318}, {505, 671, 485, 319}});
    for (size_t i = 0; i < slots.size(); ++i)
    {
        auto g = slots[i];
        CHECK(
            f.layout().drop_target_index(4, area, tiling(LayoutStrategy::MasterStack), g.x + g.width / 2, g.y + g.height / 2)
            == i
        );
    }
    auto master = f.layout().hit_test(4, area, tiling(LayoutStrategy::MasterStack), 500, 250);
    REQUIRE(master);
    CHECK(master->address == SplitAddress{ 0 });
    CHECK(master->direction == SplitDirection::Horizontal);
    CHECK(master->available_extent == 970);
    auto stack = f.layout().hit_test(4, area, tiling(LayoutStrategy::MasterStack), 750, 339);
    REQUIRE(stack);
    CHECK(stack->address == SplitAddress{ 1 });
    CHECK(stack->direction == SplitDirection::Vertical);
    CHECK_THAT(stack->ratio, Catch::Matchers::WithinAbs(1.0 / 3, 1e-12));
    CHECK_FALSE(f.layout().hit_test(4, area, tiling(LayoutStrategy::MasterStack), 100, 250));
    CHECK_FALSE(f.layout().hit_test(4, area, tiling(LayoutStrategy::MasterStack), 500, -20));
}

TEST_CASE("Saved ratios affect arrangement and hit testing consistently", "[layout]")
{
    LayoutFixture f;
    Geometry area{ 0, 0, 1000, 1000 };
    SplitRatioMap ratios{
        { SplitAddress{ 0 }, 0.7 },
        { SplitAddress{ 1 }, 0.3 }
    };
    auto slots = f.layout().arrange(4, area, tiling(LayoutStrategy::MasterStack, ratios));
    CHECK(slots[0].width == 679);
    CHECK(slots[1].height == 291);
    auto hit = f.layout().hit_test(4, area, tiling(LayoutStrategy::MasterStack, ratios), 800, 307);
    REQUIRE(hit);
    CHECK(hit->address == SplitAddress{ 1 });
    CHECK(hit->ratio == 0.3);
    // The first stack split keeps its identity when a window is added.
    auto more = f.layout().arrange(5, area, tiling(LayoutStrategy::MasterStack, ratios));
    CHECK(more[1] == slots[1]);
}

TEST_CASE("Large layouts have distinct resizable splits beyond the old path limit", "[layout]")
{
    LayoutFixture f;
    f.padding = 0;
    f.config.resize_grab_threshold = 0;
    for (size_t count : { 34, 65, 258, 1000 })
    {
        CAPTURE(count);
        Geometry area{ 0, 0, 1000, 30000 };
        auto slots = f.layout().arrange(count, area, tiling(LayoutStrategy::MasterStack));
        REQUIRE(slots.size() == count);
        for (size_t i = 1; i + 1 < count; ++i)
        {
            auto hit =
                f.layout().hit_test(count, area, tiling(LayoutStrategy::MasterStack), 750, slots[i].y + slots[i].height);
            REQUIRE(hit);
            REQUIRE(hit->address.index == i);
            REQUIRE(slots[i].height > 0);
        }
        CHECK(slots.back().y + slots.back().height == 30000);
    }
}

TEST_CASE("Tiny workareas preserve nonzero window sizes", "[layout]")
{
    LayoutFixture f;
    auto slots = f.layout().arrange(40, { 0, 0, 1, 1 }, tiling(LayoutStrategy::MasterStack));
    REQUIRE(slots.size() == 40);
    for (auto g : slots)
    {
        CHECK(g.width >= 1);
        CHECK(g.height >= 1);
    }
}

TEST_CASE("Extreme padding and dock struts saturate instead of wrapping", "[layout][bounds]")
{
    LayoutFixture f;
    f.padding = 65535;
    auto slots = f.layout().arrange(3, { 100, 100, 1000, 1000 }, tiling(LayoutStrategy::MasterStack));
    REQUIRE(slots.size() == 3);
    for (auto g : slots)
    {
        CHECK(g.x == 32767);
        CHECK(g.y == 32767);
        CHECK(g.width == 1);
        CHECK(g.height == 1);
    }
    Monitor monitor;
    monitor.geometry.width = 1000;
    monitor.geometry.height = 800;
    monitor.strut = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    CHECK(monitor.working_area() == Geometry{ 0, 0, 1, 1 });
}
