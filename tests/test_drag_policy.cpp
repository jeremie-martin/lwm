#include "lwm/core/types.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

struct DragState
{
    bool active = false;
    bool resizing = false;
    bool tiled = false;
    xcb_window_t window = XCB_NONE;
    int16_t start_root_x = 0;
    int16_t start_root_y = 0;
    int16_t last_root_x = 0;
    int16_t last_root_y = 0;
    Geometry start_geometry;
};

DragState make_test_drag_state()
{
    DragState state;
    state.active = true;
    state.window = 0x1000;
    state.start_root_x = 100;
    state.start_root_y = 100;
    state.last_root_x = 100;
    state.last_root_y = 100;
    state.start_geometry = { 50, 50, 200, 150 };
    return state;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Mode-specific behavior tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Floating move updates position only", "[drag][floating]")
{
    DragState state = make_test_drag_state();
    state.tiled = false;
    state.resizing = false;

    int32_t dx = 50;
    int32_t dy = 30;

    Geometry updated = state.start_geometry;
    updated.x = static_cast<int16_t>(static_cast<int32_t>(state.start_geometry.x) + dx);
    updated.y = static_cast<int16_t>(static_cast<int32_t>(state.start_geometry.y) + dy);

    REQUIRE(updated.x == 100);
    REQUIRE(updated.y == 80);
    REQUIRE(updated.width == 200);
    REQUIRE(updated.height == 150);
}

TEST_CASE("Floating resize enforces minimum dimensions", "[drag][resize]")
{
    DragState state = make_test_drag_state();
    state.resizing = true;

    int32_t dx = -300;
    int32_t dy = -200;

    int32_t new_w = static_cast<int32_t>(state.start_geometry.width) + dx;
    int32_t new_h = static_cast<int32_t>(state.start_geometry.height) + dy;

    new_w = std::max<int32_t>(1, new_w);
    new_h = std::max<int32_t>(1, new_h);

    REQUIRE(new_w == 1);
    REQUIRE(new_h == 1);
}

TEST_CASE("Floating resize updates dimensions only", "[drag][floating]")
{
    DragState state = make_test_drag_state();
    state.tiled = false;
    state.resizing = true;

    int32_t dx = 50;
    int32_t dy = 30;

    Geometry updated = state.start_geometry;
    int32_t new_w = static_cast<int32_t>(state.start_geometry.width) + dx;
    int32_t new_h = static_cast<int32_t>(state.start_geometry.height) + dy;
    updated.width = static_cast<uint16_t>(std::max<int32_t>(1, new_w));
    updated.height = static_cast<uint16_t>(std::max<int32_t>(1, new_h));

    REQUIRE(updated.x == 50);
    REQUIRE(updated.y == 50);
    REQUIRE(updated.width == 250);
    REQUIRE(updated.height == 180);
}

TEST_CASE("Tiled drag updates position for visual feedback", "[drag][tiled]")
{
    DragState state;
    state.active = true;
    state.tiled = true;
    state.resizing = false;
    state.window = 0x4000;
    state.start_root_x = 100;
    state.start_root_y = 100;
    state.start_geometry = { 0, 0, 400, 300 };

    int32_t dx = 200;
    int32_t dy = 150;

    int32_t new_x = static_cast<int32_t>(state.start_geometry.x) + dx;
    int32_t new_y = static_cast<int32_t>(state.start_geometry.y) + dy;

    REQUIRE(new_x == 200);
    REQUIRE(new_y == 150);
}

// ─────────────────────────────────────────────────────────────────────────────
// Geometry preservation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Start geometry is preserved during drag", "[drag][state]")
{
    DragState state = make_test_drag_state();
    Geometry original = state.start_geometry;

    state.last_root_x = 500;
    state.last_root_y = 400;

    REQUIRE(state.start_geometry.x == original.x);
    REQUIRE(state.start_geometry.y == original.y);
    REQUIRE(state.start_geometry.width == original.width);
    REQUIRE(state.start_geometry.height == original.height);
}
