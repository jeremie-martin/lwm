#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/types.hpp"
#include <functional>
#include <vector>

namespace lwm {

namespace layout_policy {

std::vector<Geometry> calculate_slots(
    size_t count,
    Geometry const& geometry,
    AppearanceConfig const& appearance
);
size_t drop_target_index(
    size_t count,
    Geometry const& geometry,
    AppearanceConfig const& appearance,
    int16_t x,
    int16_t y
);

} // namespace layout_policy

class Layout
{
public:
    Layout(Connection& conn, AppearanceConfig const& appearance);

    void arrange(std::vector<xcb_window_t> const& windows, Geometry const& geometry);
    std::vector<Geometry> calculate_slots(size_t count, Geometry const& geometry) const;
    size_t drop_target_index(size_t count, Geometry const& geometry, int16_t x, int16_t y) const;
    void apply_size_hints(xcb_window_t window, uint32_t& width, uint32_t& height) const;
    void set_sync_request_callback(std::function<void(xcb_window_t)> callback);

private:
    Connection& conn_;
    AppearanceConfig const& appearance_;
    std::function<void(xcb_window_t)> sync_request_;

    void configure_window(xcb_window_t window, int32_t x, int32_t y, uint32_t width, uint32_t height);
};

} // namespace lwm
