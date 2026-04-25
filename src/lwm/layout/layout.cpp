#include "layout.hpp"
#include "lwm/layout/strategy.hpp"
#include <limits>

namespace {

size_t nearest_slot_index(std::vector<lwm::Geometry> const& slots, int16_t x, int16_t y)
{
    size_t best_idx = 0;
    int64_t best_dist = std::numeric_limits<int64_t>::max();
    int32_t px = x;
    int32_t py = y;

    for (size_t i = 0; i < slots.size(); ++i)
    {
        auto const& slot = slots[i];
        int32_t left = slot.x;
        int32_t top = slot.y;
        int32_t right = slot.x + static_cast<int32_t>(slot.width);
        int32_t bottom = slot.y + static_cast<int32_t>(slot.height);

        int32_t dx = (px < left) ? left - px : (px > right) ? px - right : 0;
        int32_t dy = (py < top) ? top - py : (py > bottom) ? py - bottom : 0;

        int64_t dist = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
        if (dist < best_dist)
        {
            best_dist = dist;
            best_idx = i;
        }
    }

    return best_idx;
}

} // namespace

namespace lwm {

Layout::Layout(Connection& conn, AppearanceConfig const& appearance, LayoutConfig const& layout_config)
    : conn_(conn)
    , appearance_(appearance)
    , layout_config_(layout_config)
{ }

TreeNode Layout::prepare_tree(LayoutStrategy strategy, size_t count, SplitRatioMap const& ratios) const
{
    auto tree = build_layout_tree(strategy, count, layout_config_.default_ratio);
    overlay_ratios(tree, ratios);
    return tree;
}

Geometry Layout::content_rect(Geometry const& working_area) const
{
    return working_area_to_content_rect(working_area, appearance_.padding, appearance_.border_width);
}

std::vector<Geometry> Layout::arrange(
    std::vector<xcb_window_t> const& windows,
    Geometry const& geometry,
    LayoutStrategy strategy,
    SplitRatioMap const& ratios,
    std::vector<Geometry> const* old_geometries)
{
    if (windows.empty())
        return {};

    auto tree = prepare_tree(strategy, windows.size(), ratios);
    auto cr = content_rect(geometry);
    auto slots = compute_geometries(tree, cr, appearance_.padding, appearance_.border_width);

    // Apply to X windows (skip unchanged geometries when old_geometries is provided)
    for (size_t i = 0; i < windows.size() && i < slots.size(); ++i)
    {
        uint32_t width = slots[i].width;
        uint32_t height = slots[i].height;
        apply_size_hints(windows[i], width, height);
        slots[i].width = static_cast<uint16_t>(width);
        slots[i].height = static_cast<uint16_t>(height);

        if (old_geometries && i < old_geometries->size())
        {
            auto const& old = (*old_geometries)[i];
            if (old.x == slots[i].x && old.y == slots[i].y
                && old.width == slots[i].width && old.height == slots[i].height)
                continue;
        }

        configure_window(windows[i], slots[i].x, slots[i].y, width, height);
    }

    conn_.flush();
    return slots;
}


size_t
Layout::drop_target_index(
    size_t count, Geometry const& geometry, LayoutStrategy strategy,
    SplitRatioMap const& ratios, int16_t x, int16_t y) const
{
    if (count == 0)
        return 0;

    auto tree = prepare_tree(strategy, count, ratios);
    auto cr = content_rect(geometry);
    auto slots = compute_geometries(tree, cr, appearance_.padding, appearance_.border_width);
    return nearest_slot_index(slots, x, y);
}

std::optional<SplitHitResult> Layout::hit_test(
    size_t window_count,
    Geometry const& working_area,
    LayoutStrategy strategy,
    SplitRatioMap const& ratios,
    int16_t x,
    int16_t y) const
{
    if (window_count < 2)
        return std::nullopt;

    auto tree = prepare_tree(strategy, window_count, ratios);
    auto cr = content_rect(working_area);
    return hit_test_split(
        tree, cr, appearance_.padding, appearance_.border_width,
        x, y, layout_config_.resize_grab_threshold);
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
    // For tiled windows, we intentionally ignore ALL size hints including minimum size.
    // The WM controls tiled window geometry completely. If a window specifies a minimum
    // size larger than its allocated slot, honoring it causes overlap with other windows.
    // This matches the behavior of other tiling WMs (DWM, i3, bspwm).
    //
    // Applications should handle being smaller than their preferred size gracefully.
    // Most modern toolkits (Qt, GTK) do this by scrolling, truncating, or adapting layout.
    (void)window; // Unused - kept for potential future per-window logic

    width = std::max<uint32_t>(1, width);
    height = std::max<uint32_t>(1, height);
}

} // namespace lwm
