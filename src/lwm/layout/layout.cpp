#include "layout.hpp"

namespace lwm {

Layout::Layout(Connection& conn, AppearanceConfig const& appearance)
    : conn_(conn)
    , appearance_(appearance)
{ }

void Layout::arrange(std::vector<Window> const& windows, Geometry const& geometry, bool has_internal_bar)
{
    // Map all windows
    for (auto const& window : windows)
    {
        xcb_map_window(conn_.get(), window.id);
    }

    if (windows.empty())
        return;

    // Minimum window dimensions to prevent underflow
    constexpr uint32_t MIN_DIM = 50;

    int32_t baseX = geometry.x;
    int32_t baseY = geometry.y;
    uint32_t screenWidth = geometry.width;
    uint32_t padding = appearance_.padding;

    // Only account for internal bar if it's enabled
    // External docks (Polybar) are handled via struts in working_area()
    uint32_t barHeight = has_internal_bar ? appearance_.status_bar_height : 0;
    uint32_t screenHeight = (geometry.height > barHeight) ? geometry.height - barHeight : MIN_DIM;

    // Helper to safely subtract with minimum bound
    auto safe_sub = [](uint32_t a, uint32_t b) -> uint32_t { return (a > b) ? std::max(a - b, 50u) : 50u; };

    if (windows.size() == 1)
    {
        configure_window(
            windows[0].id,
            baseX + padding,
            baseY + padding + barHeight,
            safe_sub(screenWidth, 2 * padding),
            safe_sub(screenHeight, 2 * padding)
        );
    }
    else if (windows.size() == 2)
    {
        uint32_t halfWidth = safe_sub(screenWidth, 3 * padding) / 2;
        uint32_t winHeight = safe_sub(screenHeight, 2 * padding);

        configure_window(windows[0].id, baseX + padding, baseY + padding + barHeight, halfWidth, winHeight);
        configure_window(
            windows[1].id,
            baseX + padding + halfWidth + padding,
            baseY + padding + barHeight,
            halfWidth,
            winHeight
        );
    }
    else
    {
        // Master-stack layout: first window on left half, rest stacked on right
        uint32_t halfWidth = safe_sub(screenWidth, 3 * padding) / 2;
        uint32_t masterHeight = safe_sub(screenHeight, 2 * padding);

        configure_window(windows[0].id, baseX + padding, baseY + padding + barHeight, halfWidth, masterHeight);

        size_t stackCount = windows.size() - 1;
        uint32_t totalStackPadding = static_cast<uint32_t>((stackCount + 1) * padding);
        uint32_t stackHeight = safe_sub(screenHeight, totalStackPadding) / stackCount;

        for (size_t i = 1; i < windows.size(); ++i)
        {
            configure_window(
                windows[i].id,
                baseX + padding + halfWidth + padding,
                baseY + padding + barHeight + static_cast<int32_t>((i - 1) * (stackHeight + padding)),
                halfWidth,
                stackHeight
            );
        }
    }

    conn_.flush();
}

void Layout::configure_window(xcb_window_t window, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    uint32_t values[] = { static_cast<uint32_t>(x), static_cast<uint32_t>(y), width, height };
    xcb_configure_window(
        conn_.get(),
        window,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        values
    );
}

} // namespace lwm
