#include "ipc.hpp"
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <unistd.h>

namespace lwm::ipc {

namespace {

std::string current_display()
{
    if (char const* display = std::getenv("DISPLAY"))
        return display;
    return "default";
}

std::filesystem::path fallback_runtime_directory()
{
    uid_t uid = getuid();
    return std::filesystem::path("/tmp") / ("lwm-" + std::to_string(uid));
}

}

std::string sanitize_display_name(std::string_view display)
{
    std::string sanitized;
    sanitized.reserve(display.size());

    for (char ch : display)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.'
            || ch == '_' || ch == '-')
        {
            sanitized.push_back(ch);
        }
        else
        {
            sanitized.push_back('_');
        }
    }

    if (sanitized.empty())
        return "default";

    return sanitized;
}

std::filesystem::path socket_directory()
{
    if (char const* runtime_dir = std::getenv("XDG_RUNTIME_DIR"))
    {
        return std::filesystem::path(runtime_dir) / "lwm";
    }

    return fallback_runtime_directory();
}

std::filesystem::path default_socket_path()
{
    return socket_directory() / ("ipc-" + sanitize_display_name(current_display()) + ".sock");
}

xcb_atom_t intern_atom(xcb_connection_t* conn, char const* name)
{
    auto cookie = xcb_intern_atom(conn, 0, static_cast<uint16_t>(std::strlen(name)), name);
    auto* reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    if (!reply)
        return XCB_NONE;

    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

std::optional<std::string> get_root_text_property(xcb_connection_t* conn, xcb_window_t root, char const* atom_name)
{
    xcb_atom_t property = intern_atom(conn, atom_name);
    if (property == XCB_NONE)
        return std::nullopt;

    xcb_atom_t utf8_string = intern_atom(conn, "UTF8_STRING");
    xcb_atom_t type = utf8_string != XCB_NONE ? utf8_string : XCB_ATOM_STRING;
    auto cookie = xcb_get_property(conn, 0, root, property, type, 0, 4096);
    auto* reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply)
        return std::nullopt;

    std::optional<std::string> value;
    int length = xcb_get_property_value_length(reply);
    if (length > 0)
    {
        auto const* bytes = static_cast<char const*>(xcb_get_property_value(reply));
        value = std::string(bytes, bytes + length);
    }

    free(reply);
    return value;
}

void set_root_text_property(
    xcb_connection_t* conn,
    xcb_window_t root,
    xcb_atom_t property,
    xcb_atom_t utf8_string,
    std::string const& value
)
{
    xcb_atom_t type = utf8_string != XCB_NONE ? utf8_string : XCB_ATOM_STRING;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, property, type, 8, value.size(), value.c_str());
}

void delete_root_property(xcb_connection_t* conn, xcb_window_t root, xcb_atom_t property)
{
    if (property != XCB_NONE)
        xcb_delete_property(conn, root, property);
}

} // namespace lwm::ipc
