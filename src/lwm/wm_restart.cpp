// Exec handoff: the outgoing WM stores one snapshot on the root window and
// retains its X resources until the replacement claims the screen.

#include "lwm/core/log.hpp"
#include "lwm/core/xproperty.hpp"
#include "wm.hpp"

namespace lwm {

void WindowManager::read_handoff()
{
    auto root = conn_.screen()->root;
    auto words = xproperty::read_words(conn_.get(), root, atoms_.lwm_restart, XCB_ATOM_CARDINAL, 1U << 24);
    xcb_delete_property(conn_.get(), root, atoms_.lwm_restart);
    if (words.empty())
        return;
    handoff_ = restart::decode(words);
    if (!handoff_)
        LWM_LOG_WARN(
            "Ignoring restart state: reason={} format={} expected={}; adopting windows afresh",
            words.front() != restart::format ? "format mismatch" : "malformed snapshot",
            words.front(),
            restart::format
        );
    else
        LWM_LOG_INFO("Restart state accepted: format={} monitors={} clients={} focused_monitor={} active_window={:#x}",
                     words.front(), handoff_->monitors.size(), handoff_->clients.size(), handoff_->focused_monitor, handoff_->active);
}

void WindowManager::prepare_restart()
{
    state_.end_drag(false);
    release_pointer();
    auto root = conn_.screen()->root;
    auto words = restart::encode(state_.snapshot());
    xcb_change_property(
        conn_.get(),
        XCB_PROP_MODE_REPLACE,
        root,
        atoms_.lwm_restart,
        XCB_ATOM_CARDINAL,
        32,
        static_cast<uint32_t>(words.size()),
        words.data()
    );
    LWM_LOG_INFO("Restart state serialized: clients={} words={}", state_.clients().size(), words.size());

    // Hidden windows return on-screen so they stay recoverable if exec fails;
    // the successor publishes their visibility again.
    for (auto const& [window, client] : state_.clients())
        if (auto it = outputs_.find(window); it != outputs_.end() && it->second.hidden)
        {
            int16_t x = state_.frame(client).x;
            uint32_t value = static_cast<uint16_t>(x <= OFF_SCREEN_X / 2 ? 0 : x);
            xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_X, &value);
        }

    xcb_ungrab_key(conn_.get(), XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);
    xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, root, XCB_MOD_MASK_ANY);
    for (auto const& [window, client] : state_.clients())
        xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, window, XCB_MOD_MASK_ANY);
    // Releasing SubstructureRedirect lets the successor claim the screen.
    uint32_t no_events = XCB_EVENT_MASK_NO_EVENT;
    xcb_change_window_attributes(conn_.get(), root, XCB_CW_EVENT_MASK, &no_events);
    xcb_set_selection_owner(conn_.get(), XCB_NONE, atoms_.wm_s0, XCB_CURRENT_TIME);

    // Keep one identifiable predecessor until a replacement connection exists:
    // DestroyAll here would reset an X server whose only client is the WM.
    xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, wm_window_, atoms_.lwm_restart_owner, XCB_ATOM_WINDOW, 32, 1, &wm_window_);
    xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, root, atoms_.lwm_restart_owner, XCB_ATOM_WINDOW, 32, 1, &wm_window_);
    xcb_set_close_down_mode(conn_.get(), XCB_CLOSE_DOWN_RETAIN_PERMANENT);
    cleanup_ipc();

    // A round trip guarantees the server processed the release before exec.
    conn_.sync();
}

} // namespace lwm
