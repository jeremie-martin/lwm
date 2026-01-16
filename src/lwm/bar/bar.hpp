#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/types.hpp"
#include <string>

namespace lwm {

class StatusBar
{
public:
    StatusBar(Connection& conn, AppearanceConfig const& appearance);
    ~StatusBar();

    StatusBar(StatusBar const&) = delete;
    StatusBar& operator=(StatusBar const&) = delete;

    xcb_window_t create_for_monitor(Monitor const& monitor);
    void update(Monitor const& monitor);
    void destroy(xcb_window_t bar_window);

private:
    Connection& conn_;
    AppearanceConfig const& appearance_;
    xcb_font_t font_;
    xcb_gcontext_t gc_;

    void draw_text(xcb_window_t window, std::string const& text);
};

} // namespace lwm
