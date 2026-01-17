#include "bar.hpp"
#include <cstring>
#include <utility>

namespace lwm {

StatusBar::StatusBar(Connection& conn, AppearanceConfig const& appearance, std::vector<std::string> workspace_names)
    : conn_(conn)
    , appearance_(appearance)
    , workspace_names_(std::move(workspace_names))
    , font_(xcb_generate_id(conn_.get()))
    , gc_(xcb_generate_id(conn_.get()))
{
    xcb_open_font(conn_.get(), font_, strlen("fixed"), "fixed");

    uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT;
    uint32_t values[3] = { conn_.screen()->black_pixel, appearance_.status_bar_color, font_ };
    xcb_create_gc(conn_.get(), gc_, conn_.screen()->root, mask, values);
}

StatusBar::~StatusBar()
{
    xcb_free_gc(conn_.get(), gc_);
    xcb_close_font(conn_.get(), font_);
}

xcb_window_t StatusBar::create_for_monitor(Monitor const& monitor)
{
    uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_ENTER_WINDOW
        | XCB_EVENT_MASK_POINTER_MOTION;
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t values[3] = { appearance_.status_bar_color, 1, event_mask };

    xcb_window_t window = xcb_generate_id(conn_.get());
    xcb_create_window(
        conn_.get(),
        XCB_COPY_FROM_PARENT,
        window,
        conn_.screen()->root,
        monitor.x,
        monitor.y,
        monitor.width,
        appearance_.status_bar_height,
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        conn_.screen()->root_visual,
        mask,
        values
    );

    xcb_map_window(conn_.get(), window);
    conn_.flush();

    return window;
}

void StatusBar::update(Monitor const& monitor)
{
    if (monitor.bar_window == XCB_NONE)
        return;

    std::string statusText = "WS: ";
    for (size_t i = 0; i < monitor.workspaces.size(); ++i)
    {
        std::string label = (i < workspace_names_.size()) ? workspace_names_[i] : std::to_string(i + 1);
        if (i == monitor.current_workspace)
        {
            statusText += "[" + label + "]";
        }
        else if (!monitor.workspaces[i].windows.empty())
        {
            statusText += " " + label + " ";
        }
        else
        {
            statusText += " · ";
        }
    }

    statusText += " | ";
    if (!monitor.name.empty())
    {
        statusText += monitor.name + " | ";
    }

    statusText += "Focused: ";
    auto const& ws = monitor.current();
    if (ws.focused_window != XCB_NONE)
    {
        auto it = ws.find_window(ws.focused_window);
        if (it != ws.windows.end())
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

    draw_text(monitor.bar_window, statusText);
}

void StatusBar::destroy(xcb_window_t bar_window)
{
    xcb_destroy_window(conn_.get(), bar_window);
    conn_.flush();
}

void StatusBar::draw_text(xcb_window_t window, std::string const& text)
{
    xcb_clear_area(conn_.get(), 0, window, 0, 0, 0, 0);
    xcb_image_text_8(conn_.get(), text.length(), window, gc_, 10, appearance_.status_bar_height - 5, text.c_str());
    conn_.flush();
}

} // namespace lwm
