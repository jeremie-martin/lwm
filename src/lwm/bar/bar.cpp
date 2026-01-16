#include "bar.hpp"
#include <algorithm>
#include <cstring>

namespace lwm
{

StatusBar::StatusBar(Connection& conn, AppearanceConfig const& appearance, int num_tags)
    : conn_(conn)
    , appearance_(appearance)
    , num_tags_(num_tags)
    , window_(XCB_NONE)
{
    create();
}

void StatusBar::create()
{
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = { appearance_.status_bar_color, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS };

    window_ = xcb_generate_id(conn_.get());
    xcb_create_window(
        conn_.get(),
        XCB_COPY_FROM_PARENT,
        window_,
        conn_.screen()->root,
        0,
        0,
        conn_.screen()->width_in_pixels,
        appearance_.status_bar_height,
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        conn_.screen()->root_visual,
        mask,
        values
    );

    xcb_map_window(conn_.get(), window_);
    conn_.flush();
}

void StatusBar::update(size_t current_tag, std::vector<Tag> const& tags)
{
    std::string statusText = "Tags: ";
    for (int i = 0; i < num_tags_; ++i)
    {
        statusText +=
            (static_cast<size_t>(i) == current_tag ? "[" : " ") + std::to_string(i) + (static_cast<size_t>(i) == current_tag ? "]" : " ");
    }

    statusText += " | Focused: ";
    if (current_tag < tags.size() && tags[current_tag].focusedWindow != XCB_NONE)
    {
        auto it = std::ranges::find_if(
            tags[current_tag].windows,
            [&tags, current_tag](Window const& w) { return w.id == tags[current_tag].focusedWindow; }
        );
        if (it != tags[current_tag].windows.end())
        {
            statusText += it->name;
        }
        else
        {
            statusText += "Unknown";
        }
    }
    else
    {
        statusText += "None";
    }

    draw_text(statusText);
}

void StatusBar::draw_text(std::string const& text)
{
    xcb_clear_area(conn_.get(), 0, window_, 0, 0, 0, 0);

    xcb_font_t font = xcb_generate_id(conn_.get());
    xcb_open_font(conn_.get(), font, strlen("fixed"), "fixed");

    xcb_gcontext_t gc = xcb_generate_id(conn_.get());
    uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT;
    uint32_t values[3] = { conn_.screen()->black_pixel, appearance_.status_bar_color, font };
    xcb_create_gc(conn_.get(), gc, window_, mask, values);

    xcb_image_text_8(conn_.get(), text.length(), window_, gc, 10, appearance_.status_bar_height - 5, text.c_str());

    xcb_close_font(conn_.get(), font);
    xcb_free_gc(conn_.get(), gc);

    conn_.flush();
}

} // namespace lwm
