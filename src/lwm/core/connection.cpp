#include "connection.hpp"
#include <stdexcept>

namespace lwm {

Connection::Connection()
    : conn_(xcb_connect(nullptr, nullptr), xcb_disconnect)
    , screen_(nullptr)
    , keysyms_(nullptr, xcb_key_symbols_free)
{
    if (xcb_connection_has_error(conn_.get()))
    {
        throw std::runtime_error("Failed to connect to X server");
    }

    screen_ = xcb_setup_roots_iterator(xcb_get_setup(conn_.get())).data;
    if (!screen_)
    {
        throw std::runtime_error("Failed to get screen");
    }

    keysyms_.reset(xcb_key_symbols_alloc(conn_.get()));
    if (!keysyms_)
    {
        throw std::runtime_error("Failed to allocate key symbols");
    }

    init_randr();
}

void Connection::init_randr()
{
    // Sending QueryVersion also makes xcb cache the extension's event base.
    auto cookie = xcb_randr_query_version(conn_.get(), XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);
    auto* version = xcb_randr_query_version_reply(conn_.get(), cookie, nullptr);
    auto const* extension = xcb_get_extension_data(conn_.get(), &xcb_randr_id);
    randr_available_ = version && extension && extension->present;
    randr_event_base_ = randr_available_ ? extension->first_event : 0;
    free(version);
}

}
