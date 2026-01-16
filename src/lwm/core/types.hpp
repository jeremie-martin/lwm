#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <xcb/xcb.h>

namespace lwm
{

struct Window
{
    xcb_window_t id;
    std::string name;
};

struct Tag
{
    std::vector<Window> windows;
    xcb_window_t focusedWindow = XCB_NONE;
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
    int tag = -1;
};

} // namespace lwm
