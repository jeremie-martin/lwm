#include <catch2/catch_test_macros.hpp>
#include "lwm/core/classification.hpp"

using namespace lwm;

TEST_CASE("Desktop windows are classified as desktop", "[ewmh][classification]")
{
    auto result = classify_window_type(WindowType::Desktop, false);

    REQUIRE(result.role == WindowRole::Desktop);
    REQUIRE(result.skip_taskbar);
    REQUIRE(result.skip_pager);
}

TEST_CASE("Dock windows ignore transient flag", "[ewmh][classification]")
{
    auto result = classify_window_type(WindowType::Dock, true);

    REQUIRE(result.role == WindowRole::Dock);
    REQUIRE(result.skip_taskbar);
    REQUIRE(result.skip_pager);
}

TEST_CASE("Utility windows float above and skip taskbar", "[ewmh][classification]")
{
    auto result = classify_window_type(WindowType::Utility, false);

    REQUIRE((result.role == WindowRole::Client && result.floating));
    REQUIRE(result.skip_taskbar);
    REQUIRE(result.skip_pager);
    REQUIRE(result.above);
}

TEST_CASE("Dialog windows float without forcing skip flags", "[ewmh][classification]")
{
    auto result = classify_window_type(WindowType::Dialog, false);

    REQUIRE((result.role == WindowRole::Client && result.floating));
    REQUIRE_FALSE(result.skip_taskbar);
    REQUIRE_FALSE(result.skip_pager);
}

TEST_CASE("Menu, Toolbar, and Splash windows float and skip taskbar", "[ewmh][classification]")
{
    for (auto type : { WindowType::Menu, WindowType::Toolbar, WindowType::Splash })
    {
        CAPTURE(type);
        auto result = classify_window_type(type, false);
        REQUIRE((result.role == WindowRole::Client && result.floating));
        REQUIRE(result.skip_taskbar);
        REQUIRE(result.skip_pager);
        REQUIRE_FALSE(result.above);
    }
}

TEST_CASE("All popup-class types are classified as popup", "[ewmh][classification]")
{
    for (auto type : { WindowType::DropdownMenu,
                       WindowType::PopupMenu,
                       WindowType::Notification,
                       WindowType::Combo,
                       WindowType::Dnd,
                       WindowType::Tooltip })
    {
        CAPTURE(type);
        auto result = classify_window_type(type, false);
        REQUIRE(result.role == WindowRole::Popup);
        REQUIRE(result.skip_taskbar);
        REQUIRE(result.skip_pager);
    }
}

TEST_CASE("Normal windows honor transient flag", "[ewmh][classification]")
{
    auto normal = classify_window_type(WindowType::Normal, false);
    auto transient = classify_window_type(WindowType::Normal, true);

    REQUIRE((normal.role == WindowRole::Client && !normal.floating));
    REQUIRE_FALSE(normal.skip_taskbar);
    REQUIRE_FALSE(normal.skip_pager);

    REQUIRE((transient.role == WindowRole::Client && transient.floating));
    REQUIRE(transient.skip_taskbar);
    REQUIRE(transient.skip_pager);
}

TEST_CASE("Effective values let preferences override defaults and state project the layer", "[classification]")
{
    Client client;
    client.ewmh_type = WindowType::Utility;
    CHECK(effective_layer(client) == LayerHint::Above);
    CHECK(skips_taskbar(client));
    CHECK(default_floating(client) == true);

    client.preferences.layer = LayerHint::Below;
    client.preferences.skip_taskbar = false;
    CHECK(effective_layer(client) == LayerHint::Below);
    CHECK_FALSE(skips_taskbar(client));

    // Modal and fullscreen project the layer without erasing the preference.
    client.modal = true;
    CHECK(effective_layer(client) == LayerHint::Above);
    client.fullscreen = true;
    CHECK(effective_layer(client) == LayerHint::Normal);
    client.modal = client.fullscreen = false;
    CHECK(effective_layer(client) == LayerHint::Below);

    // Transients skip taskbar and pager by default, including dialogs.
    Client dialog;
    dialog.ewmh_type = WindowType::Dialog;
    CHECK_FALSE(skips_pager(dialog));
    dialog.transient_for = 42;
    CHECK(skips_pager(dialog));

    // Runtime conversion into dock, desktop or popup types has no normal default.
    Client dock;
    dock.ewmh_type = WindowType::Dock;
    CHECK_FALSE(default_floating(dock));
}
