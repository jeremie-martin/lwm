#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <xcb/xcb.h>

namespace lwm::ipc {

inline constexpr size_t max_reply_bytes = 8 * 1024 * 1024;
inline constexpr size_t max_event_bytes = 1024 * 1024;

std::string sanitize_display_name(std::string_view display);
std::filesystem::path socket_directory();
std::filesystem::path default_socket_path();

xcb_atom_t intern_atom(xcb_connection_t* conn, char const* name);
std::optional<std::string> get_root_text_property(xcb_connection_t* conn, xcb_window_t root, char const* atom_name);
void set_root_text_property(
    xcb_connection_t* conn,
    xcb_window_t root,
    xcb_atom_t property,
    xcb_atom_t utf8_string,
    std::string const& value
);
void delete_root_property(xcb_connection_t* conn, xcb_window_t root, xcb_atom_t property);

} // namespace lwm::ipc
