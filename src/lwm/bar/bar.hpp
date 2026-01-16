#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/types.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace lwm
{

class StatusBar
{
public:
    StatusBar(Connection& conn, AppearanceConfig const& appearance, int num_tags);

    void update(size_t current_tag, std::vector<Tag> const& tags);

private:
    Connection& conn_;
    AppearanceConfig const& appearance_;
    int num_tags_;
    xcb_window_t window_;

    void create();
    void draw_text(std::string const& text);
};

} // namespace lwm
