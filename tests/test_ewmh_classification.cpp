#include <catch2/catch_test_macros.hpp>
#include "lwm/core/ewmh.hpp"

using namespace lwm;

TEST_CASE("Desktop windows are classified as desktop", "[ewmh][classification]")
{
    auto result = classify_window_type(WindowType::Desktop, false);

    REQUIRE(result.kind == WindowClassification::Kind::Desktop);
    REQUIRE(result.skip_taskbar);
    REQUIRE(result.skip_pager);
    REQUIRE_FALSE(result.is_transient);
}

TEST_CASE("Dock windows ignore transient flag", "[ewmh][classification]")
{
    auto result = classify_window_type(WindowType::Dock, true);

    REQUIRE(result.kind == WindowClassification::Kind::Dock);
    REQUIRE(result.skip_taskbar);
    REQUIRE(result.skip_pager);
    REQUIRE(result.is_transient);
}

TEST_CASE("Menu windows float and skip taskbar", "[ewmh][classification]")
{
    auto result = classify_window_type(WindowType::Menu, false);

    REQUIRE(result.kind == WindowClassification::Kind::Floating);
    REQUIRE(result.skip_taskbar);
    REQUIRE(result.skip_pager);
    REQUIRE_FALSE(result.above);
}

TEST_CASE("Utility windows float above and skip taskbar", "[ewmh][classification]")
{
    auto result = classify_window_type(WindowType::Utility, false);

    REQUIRE(result.kind == WindowClassification::Kind::Floating);
    REQUIRE(result.skip_taskbar);
    REQUIRE(result.skip_pager);
    REQUIRE(result.above);
}

TEST_CASE("Dialog windows float without forcing skip flags", "[ewmh][classification]")
{
    auto result = classify_window_type(WindowType::Dialog, false);

    REQUIRE(result.kind == WindowClassification::Kind::Floating);
    REQUIRE_FALSE(result.skip_taskbar);
    REQUIRE_FALSE(result.skip_pager);
}

TEST_CASE("Popup types are classified as popup", "[ewmh][classification]")
{
    auto result = classify_window_type(WindowType::Tooltip, false);

    REQUIRE(result.kind == WindowClassification::Kind::Popup);
    REQUIRE(result.skip_taskbar);
    REQUIRE(result.skip_pager);
}

TEST_CASE("Normal windows honor transient flag", "[ewmh][classification]")
{
    auto normal = classify_window_type(WindowType::Normal, false);
    auto transient = classify_window_type(WindowType::Normal, true);

    REQUIRE(normal.kind == WindowClassification::Kind::Tiled);
    REQUIRE_FALSE(normal.skip_taskbar);
    REQUIRE_FALSE(normal.skip_pager);

    REQUIRE(transient.kind == WindowClassification::Kind::Floating);
    REQUIRE(transient.skip_taskbar);
    REQUIRE(transient.skip_pager);
    REQUIRE(transient.is_transient);
}
