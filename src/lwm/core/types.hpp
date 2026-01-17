#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <xcb/randr.h>
#include <xcb/xcb.h>

namespace lwm {

struct Window
{
    xcb_window_t id;
    std::string name;
    std::string wm_class;
    std::string wm_class_name;
};

struct Workspace
{
    std::vector<Window> windows;
    xcb_window_t focused_window = XCB_NONE;

    auto find_window(xcb_window_t id)
    {
        return std::ranges::find_if(windows, [id](Window const& w) { return w.id == id; });
    }

    auto find_window(xcb_window_t id) const
    {
        return std::ranges::find_if(windows, [id](Window const& w) { return w.id == id; });
    }
};

struct Geometry
{
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
};

struct Strut
{
    uint32_t left = 0;
    uint32_t right = 0;
    uint32_t top = 0;
    uint32_t bottom = 0;
};

struct Monitor
{
    xcb_randr_output_t output = XCB_NONE;
    std::string name;
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<Workspace> workspaces;
    size_t current_workspace = 0;
    size_t previous_workspace = 0;
    xcb_window_t bar_window = XCB_NONE;
    Strut strut = {};

    Workspace& current() { return workspaces[current_workspace]; }
    Workspace const& current() const { return workspaces[current_workspace]; }

    Geometry geometry() const { return { x, y, width, height }; }

    Geometry working_area() const
    {
        // Clamp struts to prevent underflow
        uint32_t h_strut = std::min(static_cast<uint32_t>(width), strut.left + strut.right);
        uint32_t v_strut = std::min(static_cast<uint32_t>(height), strut.top + strut.bottom);
        return { static_cast<int16_t>(x + strut.left),
                 static_cast<int16_t>(y + strut.top),
                 static_cast<uint16_t>(width - h_strut),
                 static_cast<uint16_t>(height - v_strut) };
    }
};

struct KeyBinding
{
    uint16_t modifier;
    xcb_keysym_t keysym;

    auto operator<=>(KeyBinding const&) const = default;
};

struct Action
{
    std::string type;
    std::string command;
    int workspace = -1;
};

} // namespace lwm
