#include <catch2/catch_test_macros.hpp>
#include <lwm/core/workarea.hpp>

using namespace lwm;

TEST_CASE("Root-relative dock reservations project onto monitor edges", "[workarea]")
{
    Geometry root{ 0, 0, 2304, 1024 };
    Geometry left{ 0, 0, 1280, 1024 };
    Geometry right{ 1280, 0, 1024, 768 };
    DockStrut dock;
    // EWMH's unequal-height monitor example: a 50px panel on the shorter output.
    dock.bottom = { 306, 1280, 2303 };
    CHECK(monitor_strut(dock, root, left).bottom == 0);
    CHECK(monitor_strut(dock, root, right).bottom == 50);
    dock.top = { 40, 0, 1279 };
    CHECK(monitor_strut(dock, root, left).top == 40);
    CHECK(monitor_strut(dock, root, right).top == 0);
    dock.top.end = 1280;
    CHECK(monitor_strut(dock, root, right).top == 40);
    dock.top = { 40 }; // Legacy ranges cover both monitors.
    CHECK(monitor_strut(dock, root, right).top == 40);
    dock.left = { 1300, 0, 767 };
    CHECK(monitor_strut(dock, root, right).left == 20);
    dock.right = { 1044, 0, 1023 };
    CHECK(monitor_strut(dock, root, left).right == 20);
}

TEST_CASE("Invalid and extreme dock reservations cannot wrap workareas", "[workarea]")
{
    Geometry root{ 0, 0, 1920, 1080 };
    Geometry monitor{ 100, 100, 800, 600 };
    DockStrut dock;
    dock.top = { UINT32_MAX, 0, UINT32_MAX };
    dock.bottom = dock.top;
    dock.left = dock.top;
    dock.right = dock.top;
    auto projected = monitor_strut(dock, root, monitor);
    CHECK(projected.top == 600);
    CHECK(projected.bottom == 600);
    CHECK(projected.left == 800);
    CHECK(projected.right == 800);
    dock.top = { 50, 900, 100 };
    CHECK(monitor_strut(dock, root, monitor).top == 0);
    dock.top = { 50 };
    CHECK(monitor_strut(dock, root, monitor).top == 0);
    dock.top = { 150 };
    CHECK(monitor_strut(dock, root, monitor).top == 50);
}
