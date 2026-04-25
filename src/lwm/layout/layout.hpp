#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/types.hpp"
#include "lwm/layout/split_tree.hpp"
#include <functional>
#include <optional>
#include <vector>

namespace lwm {

class Layout
{
public:
    Layout(Connection& conn, AppearanceConfig const& appearance, LayoutConfig const& layout_config);

    /// Tree-based arrange: builds tree from strategy, overlays workspace ratios, computes geometries.
    /// If old_geometries is provided, windows whose geometry hasn't changed are not reconfigured.
    std::vector<Geometry> arrange(
        std::vector<xcb_window_t> const& windows,
        Geometry const& geometry,
        LayoutStrategy strategy,
        SplitRatioMap const& ratios,
        std::vector<Geometry> const* old_geometries = nullptr);

    size_t drop_target_index(
        size_t count, Geometry const& geometry, LayoutStrategy strategy,
        SplitRatioMap const& ratios, int16_t x, int16_t y) const;
    void apply_size_hints(xcb_window_t window, uint32_t& width, uint32_t& height) const;
    void set_sync_request_callback(std::function<void(xcb_window_t)> callback);

    /// Hit-test for tiled resize: find the nearest split border to (x, y).
    std::optional<SplitHitResult> hit_test(
        size_t window_count,
        Geometry const& working_area,
        LayoutStrategy strategy,
        SplitRatioMap const& ratios,
        int16_t x,
        int16_t y) const;

    LayoutConfig const& layout_config() const { return layout_config_; }

private:
    Connection& conn_;
    AppearanceConfig const& appearance_;
    LayoutConfig const& layout_config_;
    std::function<void(xcb_window_t)> sync_request_;

    TreeNode prepare_tree(LayoutStrategy strategy, size_t count, SplitRatioMap const& ratios) const;
    Geometry content_rect(Geometry const& working_area) const;
    void configure_window(xcb_window_t window, int32_t x, int32_t y, uint32_t width, uint32_t height);
};

} // namespace lwm
