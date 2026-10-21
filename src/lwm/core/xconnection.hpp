#pragma once

#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include <xcb/xcb.h>

namespace lwm {

// Owns one reply that XCB allocated with malloc.
template <typename T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
template <typename T> Reply<T> reply(T* value) { return { value, &std::free }; }

// Interns every name in one round trip. Returns false if any lookup failed.
inline bool intern_atoms(xcb_connection_t* connection, std::span<std::pair<char const*, xcb_atom_t*> const> names)
{
    std::vector<xcb_intern_atom_cookie_t> cookies;
    for (auto const& [name, atom] : names)
        cookies.push_back(xcb_intern_atom(connection, 0, static_cast<uint16_t>(std::strlen(name)), name));
    bool interned = true;
    for (size_t i = 0; i < names.size(); ++i)
    {
        auto atom = reply(xcb_intern_atom_reply(connection, cookies[i], nullptr));
        *names[i].second = atom ? atom->atom : XCB_NONE;
        interned &= atom != nullptr;
    }
    return interned;
}

// Sends a format-32 ClientMessage about `window` to its clients selecting `mask`; an
// empty mask reaches the window's creator.
inline void send_message(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t type, uint32_t mask,
                         xcb_client_message_data_t data)
{
    xcb_client_message_event_t event{ XCB_CLIENT_MESSAGE, 32, 0, window, type, data };
    xcb_send_event(connection, 0, window, mask, reinterpret_cast<char const*>(&event));
}

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
