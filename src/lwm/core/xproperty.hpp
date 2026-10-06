#pragma once

#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <xcb/xcb.h>

namespace lwm {
// Owns one reply that XCB allocated with malloc.
template <typename T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
template <typename T> Reply<T> reply(T* value) { return { value, &std::free }; }
} // namespace lwm

namespace lwm::xproperty {
using Reply = lwm::Reply<xcb_get_property_reply_t>;

inline xcb_get_property_cookie_t
request(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint32_t limit)
{
    return xcb_get_property(connection, false, window, property, type, 0, limit);
}

inline Reply receive(xcb_connection_t* connection, xcb_get_property_cookie_t cookie)
{
    return reply(xcb_get_property_reply(connection, cookie, nullptr));
}

inline Reply
read(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint32_t limit)
{
    return receive(connection, request(connection, window, property, type, limit));
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

inline std::optional<uint32_t> scalar(xcb_connection_t* connection, xcb_get_property_cookie_t cookie, xcb_atom_t type)
{
    auto reply = receive(connection, cookie);
    auto values = words(reply, type);
    return values.size() == 1 ? std::optional{ values.front() } : std::nullopt;
}

inline std::optional<uint32_t>
scalar(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property, xcb_atom_t type)
{
    return scalar(connection, request(connection, window, property, type, 1), type);
}

// Complete 8-bit text of one type, optionally deleted by the same request.
inline std::optional<std::string>
text(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property, xcb_atom_t type, bool remove = false)
{
    auto value = reply(xcb_get_property_reply(
        connection, xcb_get_property(connection, remove, window, property, type, 0, UINT32_MAX / 4), nullptr
    ));
    if (!complete(value, type, 8))
        return std::nullopt;
    return std::string(
        static_cast<char const*>(xcb_get_property_value(value.get())),
        static_cast<size_t>(xcb_get_property_value_length(value.get()))
    );
}

} // namespace lwm::xproperty
