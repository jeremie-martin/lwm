// Exec handoff: the outgoing WM stores one snapshot on the root window and passes
// its open X connection, so the server always has a live client, until the
// replacement claims the screen.

#include "lwm/core/log.hpp"
#include "lwm/core/xproperty.hpp"
#include "wm.hpp"
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace lwm {

namespace {
constexpr char const* kPredecessorConnection = "LWM_RESTART_FD";
} // namespace

// The screen is owned now; closing the predecessor's connection frees its
// resources. Returns whether this start continues an exec restart.
bool WindowManager::release_predecessor()
{
    auto const* predecessor = std::getenv(kPredecessorConnection);
    bool handoff = predecessor != nullptr;
    if (handoff)
    {
        close(std::atoi(predecessor));
        unsetenv(kPredecessorConnection);
    }
    read_handoff();
    return handoff;
}

void WindowManager::read_handoff()
{
    auto root = conn_.screen()->root;
    auto text = xproperty::text(conn_.get(), root, atoms_.lwm_restart, ewmh_.get()->UTF8_STRING);
    xcb_delete_property(conn_.get(), root, atoms_.lwm_restart);
    if (!text)
        return;
    handoff_ = restart::decode(*text);
    if (!handoff_)
        LWM_LOG_WARN("Ignoring restart state: malformed or not format {}; adopting windows afresh", restart::format);
    else
        LWM_LOG_INFO("Restart state accepted: monitors={} clients={} focused_monitor={} active_window={:#x}",
                     handoff_->monitors.size(), handoff_->clients.size(), handoff_->focused_monitor, handoff_->active);
}

void WindowManager::prepare_restart()
{
    restore_clients();
    auto root = conn_.screen()->root;
    // A snapshot the server would refuse is dropped; the successor adopts windows afresh.
    auto text = restart::encode(state_.snapshot());
    if (write_property(root, atoms_.lwm_restart, ewmh_.get()->UTF8_STRING, 8, text))
        LWM_LOG_INFO("Restart state serialized: clients={} bytes={}", state_.clients().size(), text.size());
    else
        LWM_LOG_WARN("Restart state not saved: bytes={} exceeds the request limit", text.size());

    xcb_ungrab_key(conn_.get(), XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);
    xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, root, XCB_MOD_MASK_ANY);
    for (auto const& [window, client] : state_.clients())
        xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, window, XCB_MOD_MASK_ANY);
    // Releasing SubstructureRedirect lets the successor claim the screen.
    uint32_t no_events = XCB_EVENT_MASK_NO_EVENT;
    xcb_change_window_attributes(conn_.get(), root, XCB_CW_EVENT_MASK, &no_events);
    xcb_set_selection_owner(conn_.get(), XCB_NONE, atoms_.wm_s0, XCB_CURRENT_TIME);

    // A round trip guarantees the server processed the release before exec. The
    // connection stays open through exec, for the successor or a failed-exec
    // recovery to close.
    conn_.sync();
    setenv(kPredecessorConnection, std::to_string(conn_.detach()).c_str(), 1);
}

} // namespace lwm
