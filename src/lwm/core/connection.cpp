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
    auto cookie = xcb_randr_query_version(conn_.get(), XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);
    auto* reply = xcb_randr_query_version_reply(conn_.get(), cookie, nullptr);
    if (!reply)
        return;

    free(reply);

    auto ext_cookie = xcb_query_extension(conn_.get(), 5, "RANDR");
    auto* ext_reply = xcb_query_extension_reply(conn_.get(), ext_cookie, nullptr);
    if (!ext_reply)
        return;

    if (ext_reply->present)
    {
        randr_event_base_ = ext_reply->first_event;
        randr_available_ = true;
    }

    free(ext_reply);
}

} // namespace lwm
