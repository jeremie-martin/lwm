#pragma once

#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <xcb/xcb.h>

namespace lwm::xproperty {
using Reply = std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;

inline Reply receive(xcb_connection_t* connection, xcb_get_property_cookie_t cookie)
{
    return { xcb_get_property_reply(connection, cookie, nullptr), &std::free };
}

inline Reply
read(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint32_t limit)
{
    return receive(connection, xcb_get_property(connection, false, window, property, type, 0, limit));
}

// Callers decide the fallback; malformed, missing and incomplete numeric data
// must never be interpreted as a valid prefix. No extra X request is needed.
inline bool complete(Reply const& reply, xcb_atom_t type, uint8_t format)
{
    return reply && reply->type == type && reply->format == format && reply->bytes_after == 0;
}

inline std::span<uint32_t const> words(Reply const& reply, xcb_atom_t type)
{
    if (!complete(reply, type, 32))
        return { };
    return { static_cast<uint32_t const*>(xcb_get_property_value(reply.get())),
             static_cast<size_t>(xcb_get_property_value_length(reply.get())) / 4 };
}

inline std::optional<uint32_t>
scalar(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property, xcb_atom_t type)
{
    auto reply = read(connection, window, property, type, 1);
    auto values = words(reply, type);
    if (values.size() != 1)
        return std::nullopt;
    return values.front();
}

inline std::vector<uint32_t> read_words(
    xcb_connection_t* connection,
    xcb_window_t window,
    xcb_atom_t property,
    xcb_atom_t type,
    uint32_t limit = 65536
)
{
    auto reply = read(connection, window, property, type, limit);
    auto values = words(reply, type);
    return { values.begin(), values.end() };
}

} // namespace lwm::xproperty
