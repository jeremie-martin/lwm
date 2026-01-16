#pragma once

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

    bool has_randr() const { return randr_available_; }
    uint8_t randr_event_base() const { return randr_event_base_; }

    void flush() { xcb_flush(conn_.get()); }

private:
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> conn_;
    xcb_screen_t* screen_;
    std::unique_ptr<xcb_key_symbols_t, decltype(&xcb_key_symbols_free)> keysyms_;

    bool randr_available_ = false;
    uint8_t randr_event_base_ = 0;

    void init_randr();
};

} // namespace lwm
