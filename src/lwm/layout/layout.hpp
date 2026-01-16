#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/types.hpp"
#include <vector>

namespace lwm
{

class Layout
{
public:
    Layout(Connection& conn, AppearanceConfig const& appearance);

    void arrange(std::vector<Window> const& windows);

private:
    Connection& conn_;
    AppearanceConfig const& appearance_;

    void configure_window(xcb_window_t window, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
};

} // namespace lwm
