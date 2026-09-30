#include "lwm/core/floating.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

TEST_CASE("Floating placement centers and clamps without changing requested size", "[floating]")
{
    struct Example
    {
        char const* name;
        Geometry area;
        std::optional<Geometry> parent;
        Geometry expected;
    };
    for (auto const& example : std::vector<Example>{
             {              "center",       { 0, 0, 200, 100 },                            { },       { 75, 40, 50, 20 } },
             {        "parent clamp",       { 0, 0, 100, 100 },     Geometry{ 10, 10, 20, 20 },         { 0, 0, 80, 80 } },
             {           "oversized",        { 10, 5, 40, 30 },                            { },       { 10, 5, 120, 80 } },
             {              "offset",    { 100, 50, 800, 600 },                            { },   { 400, 300, 200, 100 } },
             {           "top strut",    { 0, 30, 1920, 1050 },                            { },   { 760, 405, 400, 300 } },
             {       "parent center",     { 0, 0, 1920, 1080 }, Geometry{ 500, 300, 400, 200 },   { 600, 350, 200, 100 } },
             {          "left clamp",     { 0, 0, 1920, 1080 },    Geometry{ 10, 500, 50, 50 },     { 0, 475, 200, 100 } },
             {         "right clamp",     { 0, 0, 1920, 1080 },  Geometry{ 1850, 500, 50, 50 },  { 1720, 475, 200, 100 } },
             {           "top clamp",     { 0, 0, 1920, 1080 },    Geometry{ 500, 10, 50, 50 },     { 425, 0, 200, 100 } },
             {        "bottom clamp",     { 0, 0, 1920, 1080 },  Geometry{ 500, 1050, 50, 50 },   { 425, 980, 200, 100 } },
             {       "larger parent",     { 0, 0, 1920, 1080 }, Geometry{ 200, 200, 800, 600 },    { 550, 475, 100, 50 } },
             {           "same size",   { 100, 100, 500, 400 },                            { },   { 100, 100, 500, 400 } },
             {          "round down",     { 0, 0, 1920, 1080 },                            { },       { 959, 539, 1, 1 } },
             {           "tiny area",       { 500, 500, 1, 1 },                            { },   { 500, 500, 100, 100 } },
             { "oversized both axes",       { 0, 0, 100, 100 },                            { },     { 0, 0, 1234, 5678 } },
             {       "right monitor",  { 1920, 0, 1920, 1080 },                            { },  { 2680, 390, 400, 300 } },
             {        "left monitor", { -1920, 0, 1920, 1080 },                            { }, { -1160, 390, 400, 300 } },
             {       "upper monitor", { 0, -1080, 1920, 1080 },                            { },  { 760, -690, 400, 300 } },
    })
    {
        CAPTURE(example.name);
        CHECK(
            floating::place_floating(example.area, example.expected.width, example.expected.height, example.parent)
            == example.expected
        );
    }
}

TEST_CASE("Floating translation keeps windows on the target monitor", "[floating]")
{
    SECTION("Preserves the relative offset between monitor work areas")
    {
        Geometry source_area{ 0, 0, 1920, 1080 };
        Geometry target_area{ 1920, 0, 1920, 1080 };
        Geometry geometry{ 120, 180, 400, 300 };

        auto translated = floating::translate_to_area(geometry, source_area, target_area);

        REQUIRE(translated.x == 2040);
        REQUIRE(translated.y == 180);
        REQUIRE(translated.width == geometry.width);
        REQUIRE(translated.height == geometry.height);
    }

    SECTION("Clamps translated geometry inside a smaller target area")
    {
        Geometry source_area{ 0, 30, 1920, 1050 };
        Geometry target_area{ 1920, 0, 1280, 720 };
        Geometry geometry{ 1600, 900, 400, 300 };

        auto translated = floating::translate_to_area(geometry, source_area, target_area);

        REQUIRE(translated.x == 2800);
        REQUIRE(translated.y == 420);
        REQUIRE(translated.width == geometry.width);
        REQUIRE(translated.height == geometry.height);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Position-hint monitor guard tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Position hint is accepted only when it lands on the target monitor", "[floating]")
{
    SECTION("Hint inside the target monitor is honored")
    {
        Geometry monitor{ 0, 0, 1920, 1080 };
        REQUIRE(floating::hint_targets_monitor(monitor, 800, 400, 300, 200));
    }

    SECTION("Hint on monitor 0 while target is the right-hand monitor is rejected")
    {
        // The file-dialog regression: app requests (10,10) but the parent lives
        // on the secondary monitor at x=1920 — the hint must be rejected so the
        // dialog falls back to parent-centering.
        Geometry target{ 1920, 0, 1920, 1080 };
        REQUIRE_FALSE(floating::hint_targets_monitor(target, 10, 10, 300, 200));
    }

    SECTION("Hint that targets the right-hand monitor is honored")
    {
        Geometry target{ 1920, 0, 1920, 1080 };
        REQUIRE(floating::hint_targets_monitor(target, 2400, 500, 300, 200));
    }
}

TEST_CASE("Position hint guard uses the window center point", "[floating]")
{
    Geometry monitor{ 0, 0, 1920, 1080 };

    SECTION("Top-left off-monitor but center on it is honored")
    {
        // x=-100, width=400 → center at x=100, inside the monitor.
        REQUIRE(floating::hint_targets_monitor(monitor, -100, 500, 400, 200));
    }

    SECTION("Center just past the right edge is rejected")
    {
        // x=1820, width=400 → center at x=2020, past right edge (1920).
        REQUIRE_FALSE(floating::hint_targets_monitor(monitor, 1820, 500, 400, 200));
    }
}

TEST_CASE("Position hint guard respects non-zero monitor origin", "[floating]")
{
    // Monitor above-and-left of the origin.
    Geometry monitor{ -1920, -1080, 1920, 1080 };

    SECTION("Center within the offset monitor is honored")
    {
        REQUIRE(floating::hint_targets_monitor(monitor, -1000, -600, 300, 200));
    }

    SECTION("Center outside the offset monitor is rejected")
    {
        REQUIRE_FALSE(floating::hint_targets_monitor(monitor, 100, 100, 300, 200));
    }
}

TEST_CASE("Runtime position hints distinguish assigned desktops from WM publication", "[floating][monitor]")
{
    Monitor left;
    left.x = 0;
    left.y = 0;
    left.width = 1920;
    left.height = 1080;

    Monitor right = left;
    right.x = 1920;

    std::vector<Monitor> monitors{ left, right };
    Geometry hinted{ 2200, 100, 400, 300 };
    Client client;
    set_floating_state(client, Geometry{ 100, 100, 400, 300 });
    client.monitor = 0;

    SECTION("An ordinary client may follow its hint to another monitor")
    {
        auto target = floating::resolve_position_hint(monitors, client.monitor, client.desktop_pinned, hinted);
        REQUIRE(target.accepted);
        REQUIRE(target.monitor == 1);
    }

    SECTION("A client-authored desktop pin constrains the hint to its assigned monitor")
    {
        client.desktop_pinned = true;
        auto target = floating::resolve_position_hint(monitors, client.monitor, client.desktop_pinned, hinted);
        REQUIRE_FALSE(target.accepted);
        REQUIRE(target.monitor == 0);
    }
}

TEST_CASE("Interactive resize clamps moving edges without displacing fixed edges", "[floating][drag]")
{
    using E = lwm::floating::ResizeEdge;
    using lwm::floating::drag_geometry;
    CHECK(drag_geometry({ 100, 200, 300, 400 }, 1000, 1000, E::Left | E::Top) == Geometry{ 399, 599, 1, 1 });
    CHECK(
        drag_geometry({ 100, 200, 300, 400 }, -100000, -100000, E::Left | E::Top)
        == Geometry{ -32768, -32768, 33168, 33368 }
    );
    CHECK(
        drag_geometry({ 30000, 30000, 60000, 60000 }, -100000, -100000, E::Left | E::Top)
        == Geometry{ 24465, 24465, 65535, 65535 }
    );
    CHECK(
        drag_geometry({ 100, 200, 300, 400 }, 100000, 100000, E::Right | E::Bottom)
        == Geometry{ 100, 200, 65535, 65535 }
    );
    CHECK(drag_geometry({ 100, 200, 300, 400 }, -100000, -100000, E::Right | E::Bottom) == Geometry{ 100, 200, 1, 1 });
    CHECK(drag_geometry({ 100, 200, 300, 400 }, 100000, -100000, E::None) == Geometry{ 32767, -32768, 300, 400 });
}
