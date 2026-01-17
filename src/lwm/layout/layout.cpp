#include "layout.hpp"
#include <xcb/xcb_icccm.h>

namespace lwm {

Layout::Layout(Connection& conn, AppearanceConfig const& appearance)
    : conn_(conn)
    , appearance_(appearance)
{ }

void Layout::arrange(std::vector<Window> const& windows, Geometry const& geometry, bool has_internal_bar)
{
    if (windows.empty())
        return;

    // Minimum window dimensions to prevent underflow
    constexpr uint32_t MIN_DIM = 50;

    int32_t baseX = geometry.x;
    int32_t baseY = geometry.y;
    uint32_t screenWidth = geometry.width;
    uint32_t padding = appearance_.padding;
    uint32_t border = appearance_.border_width;

    // Only account for internal bar if it's enabled
    // External docks (Polybar) are handled via struts in working_area()
    uint32_t barHeight = has_internal_bar ? appearance_.status_bar_height : 0;
    uint32_t screenHeight = (geometry.height > barHeight) ? geometry.height - barHeight : MIN_DIM;

    // Helper to safely subtract with minimum bound
    auto safe_sub = [](uint32_t a, uint32_t b) -> uint32_t { return (a > b) ? std::max(a - b, 50u) : 50u; };

    // In X11, window position is for client area, border is drawn outside.
    // To get uniform visual gaps, we must account for border width.
    // Visual gap = padding, so client position = base + padding + border
    // Client width = available - 2*padding - 2*border (for both sides)

    if (windows.size() == 1)
    {
        uint32_t width = safe_sub(screenWidth, 2 * padding + 2 * border);
        uint32_t height = safe_sub(screenHeight, 2 * padding + 2 * border);
        apply_size_hints(windows[0].id, width, height);
        configure_window(windows[0].id, baseX + padding + border, baseY + padding + border + barHeight, width, height);
    }
    else if (windows.size() == 2)
    {
        // Two windows side by side: padding | border | win1 | border | padding | border | win2 | border | padding
        // Total borders: 4 * border (2 per window)
        // Total padding: 3 * padding
        uint32_t totalBorders = 4 * border;
        uint32_t totalPadding = 3 * padding;
        uint32_t availWidth = safe_sub(screenWidth, totalPadding + totalBorders);
        uint32_t halfWidth = availWidth / 2;
        uint32_t winHeight = safe_sub(screenHeight, 2 * padding + 2 * border);

        uint32_t leftWidth = halfWidth;
        uint32_t leftHeight = winHeight;
        apply_size_hints(windows[0].id, leftWidth, leftHeight);
        int32_t leftX = baseX + padding + border;
        int32_t leftY = baseY + padding + border + barHeight;
        configure_window(windows[0].id, leftX, leftY, leftWidth, leftHeight);

        // Right window position accounts for left window's actual size + borders
        uint32_t rightWidth = safe_sub(availWidth, leftWidth);
        uint32_t rightHeight = winHeight;
        apply_size_hints(windows[1].id, rightWidth, rightHeight);
        int32_t rightX = leftX + leftWidth + border + padding + border;
        configure_window(windows[1].id, rightX, leftY, rightWidth, rightHeight);
    }
    else
    {
        // Master-stack layout: master on left, stack on right
        // Horizontal: padding | border | master | border | padding | border | stack | border | padding
        uint32_t totalHBorders = 4 * border;
        uint32_t totalHPadding = 3 * padding;
        uint32_t availWidth = safe_sub(screenWidth, totalHPadding + totalHBorders);
        uint32_t halfWidth = availWidth / 2;

        uint32_t masterWidth = halfWidth;
        uint32_t masterHeight = safe_sub(screenHeight, 2 * padding + 2 * border);
        apply_size_hints(windows[0].id, masterWidth, masterHeight);
        int32_t masterX = baseX + padding + border;
        int32_t masterY = baseY + padding + border + barHeight;
        configure_window(windows[0].id, masterX, masterY, masterWidth, masterHeight);

        // Stack windows on right
        int32_t stackX = masterX + masterWidth + border + padding + border;
        uint32_t stackAvailWidth = safe_sub(availWidth, masterWidth);

        size_t stackCount = windows.size() - 1;
        // Vertical: (stackCount + 1) paddings, stackCount * 2 borders
        uint32_t totalVPadding = static_cast<uint32_t>((stackCount + 1) * padding);
        uint32_t totalVBorders = static_cast<uint32_t>(stackCount * 2 * border);
        uint32_t stackAvailHeight = safe_sub(screenHeight, totalVPadding + totalVBorders);
        uint32_t stackSlotHeight = stackAvailHeight / stackCount;

        int32_t currentY = baseY + padding + border + barHeight;
        for (size_t i = 1; i < windows.size(); ++i)
        {
            uint32_t stackWidth = stackAvailWidth;
            uint32_t stackH = stackSlotHeight;
            apply_size_hints(windows[i].id, stackWidth, stackH);
            configure_window(windows[i].id, stackX, currentY, stackWidth, stackH);
            currentY += stackH + border + padding + border;
        }
    }

    // Map all windows AFTER configuring (ensures correct geometry on map)
    for (auto const& window : windows)
    {
        xcb_map_window(conn_.get(), window.id);
    }

    conn_.flush();
}

void Layout::configure_window(xcb_window_t window, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    if (sync_request_)
    {
        sync_request_(window);
    }

    uint32_t values[] = { static_cast<uint32_t>(x), static_cast<uint32_t>(y), width, height };
    xcb_configure_window(
        conn_.get(),
        window,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        values
    );

    // Send synthetic ConfigureNotify so client knows its geometry immediately
    xcb_configure_notify_event_t ev = {};
    ev.response_type = XCB_CONFIGURE_NOTIFY;
    ev.event = window;
    ev.window = window;
    ev.x = static_cast<int16_t>(x);
    ev.y = static_cast<int16_t>(y);
    ev.width = static_cast<uint16_t>(width);
    ev.height = static_cast<uint16_t>(height);
    ev.border_width = static_cast<uint16_t>(appearance_.border_width);
    ev.above_sibling = XCB_NONE;
    ev.override_redirect = 0;

    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<char*>(&ev));
}

void Layout::set_sync_request_callback(std::function<void(xcb_window_t)> callback)
{
    sync_request_ = std::move(callback);
}

void Layout::apply_size_hints(xcb_window_t window, uint32_t& width, uint32_t& height) const
{
    xcb_size_hints_t hints;
    xcb_generic_error_t* error = nullptr;
    if (!xcb_icccm_get_wm_normal_hints_reply(
            conn_.get(),
            xcb_icccm_get_wm_normal_hints(conn_.get(), window),
            &hints,
            &error
        ))
    {
        if (error)
            free(error);
        return;
    }
    if (error)
        free(error);

    // Only enforce minimum size constraints.
    // We intentionally ignore:
    // - Resize increments (would cause gaps with terminals)
    // - Maximum size (tiling WM controls sizing)
    // - Aspect ratio (could cause unexpected gaps)
    // This ensures windows fill their allocated space completely.

    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)
    {
        width = std::max<uint32_t>(width, hints.min_width);
        height = std::max<uint32_t>(height, hints.min_height);
    }

    width = std::max<uint32_t>(1, width);
    height = std::max<uint32_t>(1, height);
}

} // namespace lwm
