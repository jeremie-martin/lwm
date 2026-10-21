#pragma once

#include <cstdlib>
#include <memory>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

namespace lwm {

class Connection
{
public:
    Connection();
    ~Connection() = default;

    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;
    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    xcb_connection_t* get() const { return conn_.get(); }
    xcb_screen_t* screen() const { return screen_; }
    xcb_key_symbols_t* keysyms() const { return keysyms_.get(); }

    uint8_t randr_event_base() const { return randr_event_base_; }

    void flush() { xcb_flush(conn_.get()); }
    // A request beyond the server's maximum length would close the connection.
    bool fits_property(size_t bytes) const
    {
        return bytes / 4 + (bytes % 4 != 0) + 7 <= xcb_get_maximum_request_length(conn_.get());
    }
    // Flush and wait until the server has processed every earlier request.
    void sync()
    {
        flush();
        free(xcb_get_input_focus_reply(conn_.get(), xcb_get_input_focus(conn_.get()), nullptr));
    }

private:
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> conn_;
    xcb_screen_t* screen_;
    std::unique_ptr<xcb_key_symbols_t, decltype(&xcb_key_symbols_free)> keysyms_;

    uint8_t randr_event_base_ = 0;

    void init_randr();
};

} // namespace lwm
