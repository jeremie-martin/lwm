// Link the real application entry point and WM. Fail at its first event-loop
// poll, after construction, without adding fault-injection hooks to production.
#include <stdexcept>
#include <xcb/xcb.h>

extern "C" xcb_generic_event_t* xcb_poll_for_event(xcb_connection_t*)
{
    throw std::runtime_error("injected event-loop failure");
}
