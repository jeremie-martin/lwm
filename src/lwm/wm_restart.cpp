// Exec handoff: the outgoing WM stores one snapshot on the root window and keeps
// its X connection open through exec, so the server always has a live client,
// until the replacement claims the screen.

#include "lwm/core/log.hpp"
#include "lwm/core/xproperty.hpp"
#include "wm.hpp"
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <unistd.h>

namespace lwm {

namespace {
// "PID FD": exec keeps the process ID, so a copy another process inherited is ignored.
constexpr char const* kPredecessorConnection = "LWM_RESTART_FD";
} // namespace

// The screen is owned now: closing the predecessor's descriptor lets the server free
// its resources, and its snapshot is taken over. Returns whether this start continues
// an exec restart.
bool WindowManager::release_predecessor()
{
    int pid = 0, fd = -1;
    auto const* value = std::getenv(kPredecessorConnection);
    bool handoff = value && std::sscanf(value, "%d %d", &pid, &fd) == 2 && pid == getpid();
    if (handoff)
        close(fd);
    unsetenv(kPredecessorConnection);
    auto text = xproperty::text(conn_.get(), conn_.screen()->root, atoms_.lwm_restart, ewmh_.get()->UTF8_STRING, true);
    if (!text)
        return handoff;
    handoff_ = restart::decode(*text);
    if (!handoff_)
        LWM_LOG_WARN("Ignoring restart state: malformed or not format {}; adopting windows afresh", restart::format);
    else
        LWM_LOG_INFO("Restart state accepted: monitors={} clients={} focused_monitor={} active_window={:#x}",
                     handoff_->monitors.size(), handoff_->clients.size(), handoff_->focused_monitor, handoff_->active);
    return handoff;
}

void WindowManager::prepare_restart()
{
    auto* c = conn_.get();
    auto root = conn_.screen()->root;
    // A snapshot the server would refuse is dropped; the successor adopts windows afresh.
    auto text = restart::encode(state_.snapshot());
    if (write_property(root, atoms_.lwm_restart, ewmh_.get()->UTF8_STRING, 8, text))
        LWM_LOG_INFO("Restart state serialized: clients={} bytes={}", state_.clients().size(), text.size());
    else
        LWM_LOG_WARN("Restart state not saved: bytes={} exceeds the request limit", text.size());

    // A successor that ignores the connection never reads it, so it keeps no grab or
    // event selection. Releasing SubstructureRedirect lets the successor claim the screen.
    uint32_t no_events = XCB_EVENT_MASK_NO_EVENT;
    xcb_ungrab_key(c, XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);
    xcb_ungrab_button(c, XCB_BUTTON_INDEX_ANY, root, XCB_MOD_MASK_ANY);
    for (auto window : root_children())
    {
        xcb_ungrab_button(c, XCB_BUTTON_INDEX_ANY, window, XCB_MOD_MASK_ANY);
        xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &no_events);
    }
    xcb_change_window_attributes(c, root, XCB_CW_EVENT_MASK, &no_events);
    xcb_randr_select_input(c, root, 0);
    xcb_set_selection_owner(c, XCB_NONE, atoms_.wm_s0, XCB_CURRENT_TIME);

    // A duplicate of the descriptor, unlike XCB's own, survives exec and keeps the
    // connection open until the successor or a failed-exec recovery closes it.
    auto handoff = std::to_string(getpid()) + ' ' + std::to_string(dup(xcb_get_file_descriptor(c)));
    setenv(kPredecessorConnection, handoff.c_str(), 1);
}

} // namespace lwm
