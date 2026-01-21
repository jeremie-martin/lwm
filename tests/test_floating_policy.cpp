#include <catch2/catch_test_macros.hpp>
#include "lwm/core/floating.hpp"

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

    // Should center within the area
    REQUIRE(placed.x == 100 + (800 - 200) / 2);  // 400
    REQUIRE(placed.y == 50 + (600 - 100) / 2);   // 300
    REQUIRE(placed.width == 200);
    REQUIRE(placed.height == 100);
}

TEST_CASE("Floating placement with strut-like offset", "[floating]")
{
    // Simulates 30px top bar strut
    Geometry area{ 0, 30, 1920, 1050 };
    auto placed = floating::place_floating(area, 400, 300, std::nullopt);

    REQUIRE(placed.x == (1920 - 400) / 2);       // 760
    REQUIRE(placed.y == 30 + (1050 - 300) / 2);  // 405
}

// ─────────────────────────────────────────────────────────────────────────────
// Parent placement tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Floating placement centers over parent within bounds", "[floating]")
{
    Geometry area{ 0, 0, 1920, 1080 };
    Geometry parent{ 500, 300, 400, 200 };
    auto placed = floating::place_floating(area, 200, 100, parent);

    // Center of parent is (700, 400), window should be centered there
    REQUIRE(placed.x == 500 + (400 - 200) / 2);  // 600
    REQUIRE(placed.y == 300 + (200 - 100) / 2);  // 350
}

TEST_CASE("Floating placement clamps to left edge when parent is near left", "[floating]")
{
    Geometry area{ 0, 0, 1920, 1080 };
    Geometry parent{ 10, 500, 50, 50 };
    auto placed = floating::place_floating(area, 200, 100, parent);

    // Would want x = 10 + (50-200)/2 = -65, but clamped to 0
    REQUIRE(placed.x == 0);
}

TEST_CASE("Floating placement clamps to right edge when parent is near right", "[floating]")
{
    Geometry area{ 0, 0, 1920, 1080 };
    Geometry parent{ 1850, 500, 50, 50 };
    auto placed = floating::place_floating(area, 200, 100, parent);

    // Would want x = 1850 + (50-200)/2 = 1775, max_x = 1920-200 = 1720
    REQUIRE(placed.x == 1720);
}

TEST_CASE("Floating placement clamps to top edge when parent is near top", "[floating]")
{
    Geometry area{ 0, 0, 1920, 1080 };
    Geometry parent{ 500, 10, 50, 50 };
    auto placed = floating::place_floating(area, 200, 100, parent);

    // Would want y = 10 + (50-100)/2 = -15, but clamped to 0
    REQUIRE(placed.y == 0);
}

TEST_CASE("Floating placement clamps to bottom edge when parent is near bottom", "[floating]")
{
    Geometry area{ 0, 0, 1920, 1080 };
    Geometry parent{ 500, 1050, 50, 50 };
    auto placed = floating::place_floating(area, 200, 100, parent);

    // Would want y = 1050 + (50-100)/2 = 1025, max_y = 1080-100 = 980
    REQUIRE(placed.y == 980);
}

TEST_CASE("Floating placement with parent larger than child", "[floating]")
{
    Geometry area{ 0, 0, 1920, 1080 };
    Geometry parent{ 200, 200, 800, 600 };
    auto placed = floating::place_floating(area, 100, 50, parent);

    // Child centers on parent
    REQUIRE(placed.x == 200 + (800 - 100) / 2);  // 550
    REQUIRE(placed.y == 200 + (600 - 50) / 2);   // 475
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge case tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Floating placement with window same size as area", "[floating]")
{
    Geometry area{ 100, 100, 500, 400 };
    auto placed = floating::place_floating(area, 500, 400, std::nullopt);

    // Exactly fits, should be at area origin
    REQUIRE(placed.x == 100);
    REQUIRE(placed.y == 100);
}

TEST_CASE("Floating placement with very small window", "[floating]")
{
    Geometry area{ 0, 0, 1920, 1080 };
    auto placed = floating::place_floating(area, 1, 1, std::nullopt);

    // Should be near center
    REQUIRE(placed.x == (1920 - 1) / 2);  // 959
    REQUIRE(placed.y == (1080 - 1) / 2);  // 539
}

TEST_CASE("Floating placement with 1x1 area", "[floating]")
{
    Geometry area{ 500, 500, 1, 1 };
    auto placed = floating::place_floating(area, 100, 100, std::nullopt);

    // Window larger than area, clamps to area origin
    REQUIRE(placed.x == 500);
    REQUIRE(placed.y == 500);
    REQUIRE(placed.width == 100);
    REQUIRE(placed.height == 100);
}

TEST_CASE("Floating placement preserves window dimensions", "[floating]")
{
    Geometry area{ 0, 0, 100, 100 };
    auto placed = floating::place_floating(area, 1234, 5678, std::nullopt);

    // Dimensions should never be modified
    REQUIRE(placed.width == 1234);
    REQUIRE(placed.height == 5678);
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-monitor simulation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Floating placement on second monitor (right)", "[floating]")
{
    // Second monitor at x=1920
    Geometry area{ 1920, 0, 1920, 1080 };
    auto placed = floating::place_floating(area, 400, 300, std::nullopt);

    REQUIRE(placed.x == 1920 + (1920 - 400) / 2);  // 2680
    REQUIRE(placed.y == (1080 - 300) / 2);         // 390
}

TEST_CASE("Floating placement on monitor with negative x (left of primary)", "[floating]")
{
    // Monitor to the left of primary
    Geometry area{ -1920, 0, 1920, 1080 };
    auto placed = floating::place_floating(area, 400, 300, std::nullopt);

    REQUIRE(placed.x == -1920 + (1920 - 400) / 2);  // -1160
    REQUIRE(placed.y == (1080 - 300) / 2);          // 390
}

TEST_CASE("Floating placement on monitor above primary (negative y)", "[floating]")
{
    // Monitor above primary
    Geometry area{ 0, -1080, 1920, 1080 };
    auto placed = floating::place_floating(area, 400, 300, std::nullopt);

    REQUIRE(placed.x == (1920 - 400) / 2);           // 760
    REQUIRE(placed.y == -1080 + (1080 - 300) / 2);   // -690
}
