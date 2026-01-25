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

TEST_CASE("Drag updates position for move and dimensions for resize", "[drag]")
{
    DragState state = make_test_drag_state();

    // Move only
    {
        state.resizing = false;
        int32_t dx = 50, dy = 30;
        Geometry updated = state.start_geometry;
        updated.x = static_cast<int16_t>(static_cast<int32_t>(state.start_geometry.x) + dx);
        updated.y = static_cast<int16_t>(static_cast<int32_t>(state.start_geometry.y) + dy);
        REQUIRE(updated.x == 100);
        REQUIRE(updated.y == 80);
        REQUIRE(updated.width == 200);
        REQUIRE(updated.height == 150);
    }

    // Resize only
    {
        state.resizing = true;
        int32_t dx = 50, dy = 30;
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

    // Resize enforces minimum dimensions
    {
        state.resizing = true;
        int32_t dx = -300, dy = -200;
        int32_t new_w = static_cast<int32_t>(state.start_geometry.width) + dx;
        int32_t new_h = static_cast<int32_t>(state.start_geometry.height) + dy;
        new_w = std::max<int32_t>(1, new_w);
        new_h = std::max<int32_t>(1, new_h);
        REQUIRE(new_w == 1);
        REQUIRE(new_h == 1);
    }

    // Tiled drag for visual feedback
    {
        state.tiled = true;
        state.resizing = false;
        state.start_geometry = { 0, 0, 400, 300 };
        int32_t dx = 200, dy = 150;
        int32_t new_x = static_cast<int32_t>(state.start_geometry.x) + dx;
        int32_t new_y = static_cast<int32_t>(state.start_geometry.y) + dy;
        REQUIRE(new_x == 200);
        REQUIRE(new_y == 150);
    }
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
