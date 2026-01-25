#include "lwm/core/floating.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

// ─────────────────────────────────────────────────────────────────────────────
// Basic placement tests
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// Non-zero area origin tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Floating placement respects non-zero area origin", "[floating]")
{
    // Area starts at (100, 50) - simulates monitor offset or strut
    Geometry area{ 100, 50, 800, 600 };
    auto placed = floating::place_floating(area, 200, 100, std::nullopt);

    // Should center within the area (accounting for origin)
    REQUIRE(placed.x == 400);
    REQUIRE(placed.y == 300);
    REQUIRE(placed.width == 200);
    REQUIRE(placed.height == 100);
}

TEST_CASE("Floating placement with strut-like offset", "[floating]")
{
    // Simulates 30px top bar strut
    Geometry area{ 0, 30, 1920, 1050 };
    auto placed = floating::place_floating(area, 400, 300, std::nullopt);

    // Window centered in available area (y accounts for strut)
    REQUIRE(placed.x == 760);
    REQUIRE(placed.y == 405);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parent placement tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Floating placement centers over parent within bounds", "[floating]")
{
    Geometry area{ 0, 0, 1920, 1080 };
    Geometry parent{ 500, 300, 400, 200 };
    auto placed = floating::place_floating(area, 200, 100, parent);

    // Window centered over parent
    REQUIRE(placed.x == 600);
    REQUIRE(placed.y == 350);
}

TEST_CASE("Floating placement clamps to area edges", "[floating]")
{
    Geometry area{ 0, 0, 1920, 1080 };

    SECTION("Clamp to left edge")
    {
        Geometry parent{ 10, 500, 50, 50 };
        auto placed = floating::place_floating(area, 200, 100, parent);
        REQUIRE(placed.x == 0);
    }

    SECTION("Clamp to right edge")
    {
        Geometry parent{ 1850, 500, 50, 50 };
        auto placed = floating::place_floating(area, 200, 100, parent);
        REQUIRE(placed.x == 1720);
    }

    SECTION("Clamp to top edge")
    {
        Geometry parent{ 500, 10, 50, 50 };
        auto placed = floating::place_floating(area, 200, 100, parent);
        REQUIRE(placed.y == 0);
    }

    SECTION("Clamp to bottom edge")
    {
        Geometry parent{ 500, 1050, 50, 50 };
        auto placed = floating::place_floating(area, 200, 100, parent);
        REQUIRE(placed.y == 980);
    }
}

TEST_CASE("Floating placement with parent larger than child", "[floating]")
{
    Geometry area{ 0, 0, 1920, 1080 };
    Geometry parent{ 200, 200, 800, 600 };
    auto placed = floating::place_floating(area, 100, 50, parent);

    // Child centered on parent
    REQUIRE(placed.x == 550);
    REQUIRE(placed.y == 475);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge case tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Floating placement handles various dimension and size edge cases", "[floating]")
{
    SECTION("Window same size as area")
    {
        Geometry area{ 100, 100, 500, 400 };
        auto placed = floating::place_floating(area, 500, 400, std::nullopt);

        REQUIRE(placed.x == 100);
        REQUIRE(placed.y == 100);
    }

    SECTION("Tiny window centers in large area")
    {
        Geometry area{ 0, 0, 1920, 1080 };
        auto placed = floating::place_floating(area, 1, 1, std::nullopt);
        REQUIRE(placed.x == 959);
        REQUIRE(placed.y == 539);
    }

    SECTION("Large window in tiny area clamps to origin, dimensions preserved")
    {
        Geometry area{ 500, 500, 1, 1 };
        auto placed = floating::place_floating(area, 100, 100, std::nullopt);
        REQUIRE(placed.x == 500);
        REQUIRE(placed.y == 500);
        REQUIRE(placed.width == 100);
        REQUIRE(placed.height == 100);
    }

    SECTION("Window dimensions preserved regardless of area size")
    {
        Geometry area{ 0, 0, 100, 100 };
        auto placed = floating::place_floating(area, 1234, 5678, std::nullopt);
        REQUIRE(placed.width == 1234);
        REQUIRE(placed.height == 5678);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-monitor simulation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Floating placement works correctly on all monitor positions", "[floating]")
{
    SECTION("Second monitor to the right (x=1920) and monitor to the left (negative x)")
    {
        Geometry right_area{ 1920, 0, 1920, 1080 };
        auto right_placed = floating::place_floating(right_area, 400, 300, std::nullopt);
        REQUIRE(right_placed.x == 2680);
        REQUIRE(right_placed.y == 390);

        Geometry left_area{ -1920, 0, 1920, 1080 };
        auto left_placed = floating::place_floating(left_area, 400, 300, std::nullopt);
        REQUIRE(left_placed.x == -1160);
        REQUIRE(left_placed.y == 390);
    }

    SECTION("Monitor above (negative y)")
    {
        Geometry area{ 0, -1080, 1920, 1080 };
        auto placed = floating::place_floating(area, 400, 300, std::nullopt);
        REQUIRE(placed.x == 760);
        REQUIRE(placed.y == -690);
    }
}
