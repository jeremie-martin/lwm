#include "layout.hpp"
#include <xcb/xcb_icccm.h>

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
        uint32_t width = safe_sub(screenWidth, 2 * padding);
        uint32_t height = safe_sub(screenHeight, 2 * padding);
        apply_size_hints(windows[0].id, width, height);
        configure_window(windows[0].id, baseX + padding, baseY + padding + barHeight, width, height);
    }
    else if (windows.size() == 2)
    {
        uint32_t halfWidth = safe_sub(screenWidth, 3 * padding) / 2;
        uint32_t winHeight = safe_sub(screenHeight, 2 * padding);

        uint32_t leftWidth = halfWidth;
        uint32_t leftHeight = winHeight;
        apply_size_hints(windows[0].id, leftWidth, leftHeight);
        configure_window(windows[0].id, baseX + padding, baseY + padding + barHeight, leftWidth, leftHeight);

        // Calculate right window width from remaining space for consistent padding
        uint32_t rightX = baseX + padding + leftWidth + padding;
        uint32_t rightWidth = safe_sub(screenWidth, leftWidth + 3 * padding);
        uint32_t rightHeight = winHeight;
        apply_size_hints(windows[1].id, rightWidth, rightHeight);
        configure_window(windows[1].id, rightX, baseY + padding + barHeight, rightWidth, rightHeight);
    }
    else
    {
        // Master-stack layout: first window on left half, rest stacked on right
        uint32_t halfWidth = safe_sub(screenWidth, 3 * padding) / 2;
        uint32_t masterHeight = safe_sub(screenHeight, 2 * padding);

        uint32_t masterWidth = halfWidth;
        uint32_t masterH = masterHeight;
        apply_size_hints(windows[0].id, masterWidth, masterH);
        configure_window(windows[0].id, baseX + padding, baseY + padding + barHeight, masterWidth, masterH);

        // Stack windows on right, positioned relative to actual master width
        int32_t stackX = baseX + padding + masterWidth + padding;
        uint32_t stackAvailWidth = safe_sub(screenWidth, masterWidth + 3 * padding);

        size_t stackCount = windows.size() - 1;
        uint32_t totalStackPadding = static_cast<uint32_t>((stackCount + 1) * padding);
        uint32_t stackSlotHeight = safe_sub(screenHeight, totalStackPadding) / stackCount;

        // Track Y position for sequential placement with consistent padding
        int32_t currentY = baseY + padding + barHeight;
        for (size_t i = 1; i < windows.size(); ++i)
        {
            uint32_t stackWidth = stackAvailWidth;
            uint32_t stackH = stackSlotHeight;
            apply_size_hints(windows[i].id, stackWidth, stackH);
            configure_window(windows[i].id, stackX, currentY, stackWidth, stackH);
            currentY += stackH + padding;
        }
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

    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)
    {
        width = std::max<uint32_t>(width, hints.min_width);
        height = std::max<uint32_t>(height, hints.min_height);
    }

    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)
    {
        if (hints.max_width > 0)
            width = std::min<uint32_t>(width, hints.max_width);
        if (hints.max_height > 0)
            height = std::min<uint32_t>(height, hints.max_height);
    }

    uint32_t base_width = 0;
    uint32_t base_height = 0;
    if (hints.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE)
    {
        base_width = hints.base_width;
        base_height = hints.base_height;
    }

    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC)
    {
        uint32_t inc_width = std::max<uint32_t>(1, hints.width_inc);
        uint32_t inc_height = std::max<uint32_t>(1, hints.height_inc);
        if (width > base_width)
        {
            width = base_width + ((width - base_width) / inc_width) * inc_width;
        }
        if (height > base_height)
        {
            height = base_height + ((height - base_height) / inc_height) * inc_height;
        }
    }

    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_ASPECT)
    {
        double min_ratio = static_cast<double>(hints.min_aspect_num) / std::max<int>(1, hints.min_aspect_den);
        double max_ratio = static_cast<double>(hints.max_aspect_num) / std::max<int>(1, hints.max_aspect_den);
        double ratio = static_cast<double>(width) / std::max<uint32_t>(1, height);

        if (ratio < min_ratio)
        {
            width = static_cast<uint32_t>(min_ratio * static_cast<double>(height));
        }
        else if (ratio > max_ratio)
        {
            height = static_cast<uint32_t>(static_cast<double>(width) / max_ratio);
        }
    }

    width = std::max<uint32_t>(1, width);
    height = std::max<uint32_t>(1, height);
}

} // namespace lwm
