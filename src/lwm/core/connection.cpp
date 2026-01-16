#include "connection.hpp"
#include <stdexcept>

namespace lwm
{

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
}

} // namespace lwm
