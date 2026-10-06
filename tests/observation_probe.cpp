// Link the real WM and interpose only XCB property delivery. The second connection
// writes after the observed value was read, independently of the WM's event mask.
#include <cstdlib>
#include <dlfcn.h>
#include <xcb/xcb.h>

namespace {
unsigned sequence = 0;
bool injected = false;
uint32_t setting(char const* name) { return std::strtoul(std::getenv(name), nullptr, 10); }
}

extern "C" xcb_get_property_cookie_t xcb_get_property(
    xcb_connection_t* connection, uint8_t remove, xcb_window_t window, xcb_atom_t property,
    xcb_atom_t type, uint32_t offset, uint32_t length
)
{
    static auto real = reinterpret_cast<decltype(&xcb_get_property)>(dlsym(RTLD_NEXT, "xcb_get_property"));
    auto cookie = real(connection, remove, window, property, type, offset, length);
    if (!injected && window == setting("LWM_TEST_WINDOW") && property == setting("LWM_TEST_PROPERTY"))
        sequence = cookie.sequence;
    return cookie;
}

extern "C" xcb_get_property_reply_t* xcb_get_property_reply(
    xcb_connection_t* connection, xcb_get_property_cookie_t cookie, xcb_generic_error_t** error
)
{
    static auto real = reinterpret_cast<decltype(&xcb_get_property_reply)>(dlsym(RTLD_NEXT, "xcb_get_property_reply"));
    auto* reply = real(connection, cookie, error);
    if (!injected && sequence && cookie.sequence == sequence)
    {
        injected = true;
        auto* writer = xcb_connect(nullptr, nullptr);
        uint32_t value = setting("LWM_TEST_VALUE");
        xcb_change_property(writer, XCB_PROP_MODE_REPLACE, setting("LWM_TEST_WINDOW"),
                            setting("LWM_TEST_PROPERTY"), setting("LWM_TEST_TYPE"), 32, 1, &value);
        std::free(xcb_get_input_focus_reply(writer, xcb_get_input_focus(writer), nullptr));
        xcb_disconnect(writer);
    }
    return reply;
}
