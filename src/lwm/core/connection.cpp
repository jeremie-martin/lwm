#include "connection.hpp"
#include "xconnection.hpp"
#include <stdexcept>

namespace lwm {

Connection::Connection()
    : conn_(connect_display())
    , screen_(xcb_setup_roots_iterator(xcb_get_setup(conn_.get())).data)
    , keysyms_(xcb_key_symbols_alloc(conn_.get()), xcb_key_symbols_free)
{
    if (!screen_)
        throw std::runtime_error("Failed to get screen");
    if (!keysyms_)
        throw std::runtime_error("Failed to allocate key symbols");
    // Sending QueryVersion also makes xcb cache the extension's event base.
    auto cookie = xcb_randr_query_version(conn_.get(), XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);
    auto version = reply(xcb_randr_query_version_reply(conn_.get(), cookie, nullptr));
    auto const* extension = xcb_get_extension_data(conn_.get(), &xcb_randr_id);
    if (!version || !extension || !extension->present)
        throw std::runtime_error("The X server lacks the RandR extension");
    randr_event_base_ = extension->first_event;
}

}
