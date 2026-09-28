#include "lwm/layout/layout.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace lwm;

namespace {
struct Fixture
{
    AppearanceConfig appearance;
    LayoutConfig config;
    Layout layout{ appearance, config };
    Fixture()
    {
        appearance.padding = 10;
        appearance.border_width = 2;
        config.default_ratio = 0.5;
        config.resize_grab_threshold = 8;
    }
};
}

TEST_CASE("Layout handles empty, single and monocle workspaces", "[layout]")
{
    Fixture f;
    Geometry area{ -100, 20, 1000, 500 };
    Geometry content{ -88, 32, 976, 476 };
    CHECK(f.layout.arrange(0, area, LayoutStrategy::MasterStack, {}).empty());
    CHECK(f.layout.arrange(1, area, LayoutStrategy::MasterStack, {}) == std::vector<Geometry>{ content });
    CHECK(f.layout.arrange(4, area, LayoutStrategy::Monocle, {}) == std::vector<Geometry>(4, content));
    CHECK_FALSE(f.layout.hit_test(1, area, LayoutStrategy::MasterStack, {}, 400, 250));
    CHECK_FALSE(f.layout.hit_test(4, area, LayoutStrategy::Monocle, {}, 400, 250));
    CHECK(f.layout.drop_target_index(4, area, LayoutStrategy::Monocle, {}, 400, 250) == 0);
}

TEST_CASE("Master-stack preserves pixel rounding and resize boundaries", "[layout]")
{
    Fixture f;
    Geometry area{ 0, 0, 1000, 1000 };
    auto slots = f.layout.arrange(4, area, LayoutStrategy::MasterStack, {});
    REQUIRE(slots == std::vector<Geometry>{
        {12, 12, 481, 976}, {507, 12, 481, 320},
        {507, 346, 481, 314}, {507, 674, 481, 314}});
    for (size_t i = 0; i < slots.size(); ++i)
    {
        auto g = slots[i];
        CHECK(
            f.layout.drop_target_index(4, area, LayoutStrategy::MasterStack, {}, g.x + g.width / 2, g.y + g.height / 2)
            == i
        );
    }
    auto master = f.layout.hit_test(4, area, LayoutStrategy::MasterStack, {}, 500, 250);
    REQUIRE(master);
    CHECK(master->address == SplitAddress{ 0 });
    CHECK(master->direction == SplitDirection::Horizontal);
    CHECK(master->available_extent == 962);
    auto stack = f.layout.hit_test(4, area, LayoutStrategy::MasterStack, {}, 750, 339);
    REQUIRE(stack);
    CHECK(stack->address == SplitAddress{ 1 });
    CHECK(stack->direction == SplitDirection::Vertical);
    CHECK_THAT(stack->ratio, Catch::Matchers::WithinAbs(1.0 / 3, 1e-12));
    CHECK_FALSE(f.layout.hit_test(4, area, LayoutStrategy::MasterStack, {}, 100, 250));
    CHECK_FALSE(f.layout.hit_test(4, area, LayoutStrategy::MasterStack, {}, 500, -20));
}

TEST_CASE("Saved ratios affect arrangement and hit testing consistently", "[layout]")
{
    Fixture f;
    Geometry area{ 0, 0, 1000, 1000 };
    SplitRatioMap ratios{
        { SplitAddress{ 0 }, 0.7 },
        { SplitAddress{ 1 }, 0.3 }
    };
    auto slots = f.layout.arrange(4, area, LayoutStrategy::MasterStack, ratios);
    CHECK(slots[0].width == 673);
    CHECK(slots[1].height == 288);
    auto hit = f.layout.hit_test(4, area, LayoutStrategy::MasterStack, ratios, 800, 307);
    REQUIRE(hit);
    CHECK(hit->address == SplitAddress{ 1 });
    CHECK(hit->ratio == 0.3);
    // The first stack split keeps its identity when a window is added.
    auto more = f.layout.arrange(5, area, LayoutStrategy::MasterStack, ratios);
    CHECK(more[1] == slots[1]);
}

TEST_CASE("Large layouts have distinct resizable splits beyond the old path limit", "[layout]")
{
    Fixture f;
    f.appearance.padding = 0;
    f.appearance.border_width = 0;
    f.config.resize_grab_threshold = 0;
    for (size_t count : { 34, 65, 258, 1000 })
    {
        CAPTURE(count);
        Geometry area{ 0, 0, 1000, 30000 };
        auto slots = f.layout.arrange(count, area, LayoutStrategy::MasterStack, {});
        REQUIRE(slots.size() == count);
        for (size_t i = 1; i + 1 < count; ++i)
        {
            auto hit =
                f.layout.hit_test(count, area, LayoutStrategy::MasterStack, {}, 750, slots[i].y + slots[i].height);
            REQUIRE(hit);
            REQUIRE(hit->address.index == i);
            REQUIRE(slots[i].height > 0);
        }
        CHECK(slots.back().y + slots.back().height == 30000);
    }
}

TEST_CASE("Tiny workareas preserve nonzero window sizes", "[layout]")
{
    Fixture f;
    for (auto g : f.layout.arrange(40, { 0, 0, 1, 1 }, LayoutStrategy::MasterStack, {}))
    {
        CHECK(g.width >= 1);
        CHECK(g.height >= 1);
    }
}

TEST_CASE("Split restart encoding preserves legacy addresses and supports large layouts", "[layout][restart]")
{
    CHECK(serialize_split_address({ 0 }).path == 0);
    CHECK(serialize_split_address({ 3 }).path == 7);
    for (uint32_t index : { 0u, 1u, 31u, 32u, 255u, 256u, 10000u })
    {
        auto wire = serialize_split_address({ index });
        CHECK(deserialize_split_address(wire.depth, wire.path) == SplitAddress{ index });
    }
    CHECK_FALSE(deserialize_split_address(1, 0));
    CHECK_FALSE(deserialize_split_address(3, 0xFFFFFFFF));
}
