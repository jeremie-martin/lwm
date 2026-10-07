#pragma once

#include <memory>
#include <stdexcept>
#include <xcb/xcb.h>

namespace lwm {

// LWM manages one X screen with RandR outputs. Never redirect another screen to it.
inline auto connect_display()
{
    int screen = 0;
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection{ xcb_connect(nullptr, &screen),
                                                                             xcb_disconnect };
    if (xcb_connection_has_error(connection.get()))
        throw std::runtime_error("Failed to connect to X server");
    if (screen != 0)
        throw std::runtime_error("LWM supports X screen 0 only; use DISPLAY without a nonzero screen selector");
    return connection;
}

} // namespace lwm
