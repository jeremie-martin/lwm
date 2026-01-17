#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace lwm {

struct BarState
{
    std::vector<uint8_t> workspace_has_windows;
    std::string focused_title;
};

class StatusBar
{
public:
    StatusBar(Connection& conn, AppearanceConfig const& appearance, std::vector<std::string> workspace_names);
    ~StatusBar();

    StatusBar(StatusBar const&) = delete;
    StatusBar& operator=(StatusBar const&) = delete;

    xcb_window_t create_for_monitor(Monitor const& monitor);
    void update(Monitor const& monitor, BarState const& state);
    void destroy(xcb_window_t bar_window);

private:
    Connection& conn_;
    AppearanceConfig const& appearance_;
    std::vector<std::string> workspace_names_;
    xcb_font_t font_;
    xcb_gcontext_t gc_;

    void draw_text(xcb_window_t window, std::string const& text);
};

} // namespace lwm
