#include <catch2/catch_test_macros.hpp>
#include "lwm/core/floating.hpp"

using namespace lwm;

TEST_CASE("Floating placement centers in area without parent", "[floating]")
{
    Geometry area{ 0, 0, 200, 100 };
    auto placed = floating::place_floating(area, 50, 20, std::nullopt);
    REQUIRE(placed.x == 75);
    REQUIRE(placed.y == 40);
    REQUIRE(placed.width == 50);
    REQUIRE(placed.height == 20);
}

TEST_CASE("Floating placement centers over parent and clamps to area", "[floating]")
{
    Geometry area{ 0, 0, 100, 100 };
    Geometry parent{ 10, 10, 20, 20 };
    auto placed = floating::place_floating(area, 80, 80, parent);
    REQUIRE(placed.x == 0);
    REQUIRE(placed.y == 0);
}

TEST_CASE("Floating placement clamps when window is larger than area", "[floating]")
{
    Geometry area{ 10, 5, 40, 30 };
    auto placed = floating::place_floating(area, 120, 80, std::nullopt);
    REQUIRE(placed.x == 10);
    REQUIRE(placed.y == 5);
    REQUIRE(placed.width == 120);
    REQUIRE(placed.height == 80);
}
