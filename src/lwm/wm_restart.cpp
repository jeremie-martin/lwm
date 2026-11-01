// Exec handoff: the outgoing WM stores one snapshot on the root window and keeps
// its X connection open through exec, so the server always has a live client,
// until the replacement claims the screen.

#include "lwm/core/log.hpp"
#include "lwm/core/xproperty.hpp"
#include "wm.hpp"
#include <cstdio>
#include <cstdlib>
#include <ranges>
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
// an exec restart. Only that handoff carries a snapshot; one left by an exec into
// another program is stale and is discarded, whatever its type.
bool WindowManager::release_predecessor()
{
    int pid = 0, fd = -1;
    auto const* value = std::getenv(kPredecessorConnection);
    bool restarted = value && std::sscanf(value, "%d %d", &pid, &fd) == 2 && pid == getpid();
    if (restarted)
        close(fd);
    unsetenv(kPredecessorConnection);
    auto root = conn_.screen()->root;
    auto text =
        restarted ? xproperty::text(conn_.get(), root, atoms_.lwm_restart, ewmh_.get()->UTF8_STRING) : std::nullopt;
    xcb_delete_property(conn_.get(), root, atoms_.lwm_restart);
    if (!text)
        return restarted;
    handoff_ = restart::decode(*text);
    if (!handoff_)
        LWM_LOG_WARN("Ignoring restart state: malformed or not format {}; adopting windows afresh", restart::format);
    else
        LWM_LOG_INFO("Restart state accepted: monitors={} clients={} focused_monitor={} active_window={:#x}",
                     handoff_->monitors.size(), handoff_->clients.size(), handoff_->focused_monitor, handoff_->active);
    return restarted;
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
    // event selection. LWM holds them only on windows it tracks, including any it
    // forgot since the last completion; run() then releases the root's buttons and
    // selection, SubstructureRedirect included.
    xcb_ungrab_key(c, XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);
    for (auto window : outputs_ | std::views::keys)
        release_window(window);
    xcb_randr_select_input(c, root, 0);
    xcb_set_selection_owner(c, XCB_NONE, atoms_.wm_s0, XCB_CURRENT_TIME);

    // A duplicate of the descriptor, unlike XCB's own, survives exec and keeps the
    // connection open until the successor or a failed-exec recovery closes it.
    auto handoff = std::to_string(getpid()) + ' ' + std::to_string(dup(xcb_get_file_descriptor(c)));
    setenv(kPredecessorConnection, handoff.c_str(), 1);
}

} // namespace lwm
