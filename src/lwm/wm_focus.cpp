#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <algorithm>
#include <xcb/xcb_icccm.h>

namespace lwm {

void WindowManager::focus_any_window(xcb_window_t window, bool record_user_time, uint32_t focus_timestamp)
{
    auto* client = get_client(window);
    if (showing_desktop_ || !client || client->monitor >= monitors_.size()
        || client->workspace >= monitors_[client->monitor].workspaces.size()
        || !focus_policy::is_focus_eligible(client->accepts_input, client->supports_take_focus)
        || (client->kind() != Client::Kind::Tiled && client->kind() != Client::Kind::Floating))
        return;
    if (client->iconic)
        deiconify_window(window, false);
    focused_monitor_ = client->monitor;
    if (!client->sticky)
        apply_workspace_switch(client->monitor, client->workspace);
    if (is_suppressed_by_fullscreen(*client))
    {
        focus_or_fallback(monitors_[client->monitor], false);
        return;
    }
    if (!effects_.previous_focus)
        effects_.previous_focus = active_window_;
    effects_.focus_time = focus_timestamp;
    active_window_ = window;
    if (client->kind() == Client::Kind::Tiled)
        workspace_policy::set_workspace_focus(monitors_[client->monitor].workspaces[client->workspace], window);
    client->mru_order = next_mru_order_++;
    if (record_user_time)
    {
        uint32_t time = focus_timestamp ? focus_timestamp : last_input_time_;
        if (time && (!client->user_time || !ewmh_policy::timestamp_is_before(time, client->user_time)))
            client->user_time = time;
    }
}

void WindowManager::clear_focus()
{
    if (!effects_.previous_focus)
        effects_.previous_focus = active_window_;
    active_window_ = XCB_NONE;
}

void WindowManager::focus_or_fallback(Monitor& monitor, bool record_user_time)
{
    auto& ws = monitor.current();

    size_t monitor_idx = monitor_index(monitor);

    LWM_LOG_DEBUG(
        "focus_or_fallback: monitor_idx={} current_ws={} ws.focused_window={:#x} "
        "ws.windows.size={} active_window_={:#x}",
        monitor_idx,
        monitor.current_workspace,
        ws.focused_window,
        ws.windows.size(),
        active_window_
    );

    if (monitor_idx >= monitors_.size())
    {
        LWM_LOG_DEBUG("focus_or_fallback: invalid monitor, clearing focus");
        clear_focus();
        return;
    }

    auto eligible = [this](xcb_window_t window) { return is_focus_candidate(window); };

    auto floating_candidates = build_floating_candidates();

    std::vector<xcb_window_t> sticky_tiled_candidates;
    sticky_tiled_candidates.reserve(ws.windows.size());
    for (size_t w = 0; w < monitor.workspaces.size(); ++w)
    {
        if (w == monitor.current_workspace)
            continue;
        for (xcb_window_t window : monitor.workspaces[w].windows)
        {
            if (require_client(window).sticky)
                sticky_tiled_candidates.push_back(window);
        }
    }

    LWM_LOG_TRACE(
        "focus_or_fallback: {} floating candidates, {} sticky tiled candidates",
        floating_candidates.size(),
        sticky_tiled_candidates.size()
    );

    auto selection = focus_policy::select_focus_candidate(
        ws,
        monitor_idx,
        monitor.current_workspace,
        sticky_tiled_candidates,
        floating_candidates,
        eligible
    );

    if (!selection)
    {
        LWM_LOG_DEBUG("focus_or_fallback: no candidate found, clearing focus");
        clear_focus();
        return;
    }

    LWM_LOG_DEBUG("focus_or_fallback: selected window={:#x} is_floating={}", selection->window, selection->is_floating);

    focus_any_window(selection->window, record_user_time);
}

void WindowManager::repair_focus_after_visibility_change(size_t preferred_monitor, bool record_user_time)
{
    if (monitors_.empty())
    {
        clear_focus();
        return;
    }

    if (auto* active = get_client(active_window_);
        active && active->monitor < monitors_.size() && is_focus_eligible(*active) && is_visible(*active))
    {
        focused_monitor_ = active->monitor;
        if (active->kind() == Client::Kind::Tiled && active->workspace < monitors_[active->monitor].workspaces.size())
            workspace_policy::set_workspace_focus(monitors_[active->monitor].workspaces[active->workspace], active->id);
        request_current_desktop_update();
        return;
    }

    size_t target_monitor = preferred_monitor;
    if (target_monitor >= monitors_.size())
        target_monitor = focused_monitor_ < monitors_.size() ? focused_monitor_ : 0;
    focus_or_fallback(monitors_[target_monitor], record_user_time);
}

bool WindowManager::is_focus_eligible(Client const& client) const
{
    // Dock/Desktop windows never take focus (EWMH).
    if (client.kind() == Client::Kind::Dock || client.kind() == Client::Kind::Desktop)
    {
        return false;
    }
    if (is_suppressed_by_fullscreen(client))
        return false;
    return focus_policy::is_focus_eligible(client.accepts_input, client.supports_take_focus);
}

void WindowManager::send_wm_take_focus(Client const& client, uint32_t timestamp)
{
    if (wm_protocols_ == XCB_NONE || wm_take_focus_ == XCB_NONE)
        return;

    if (!client.supports_take_focus)
        return;

    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = client.id;
    ev.type = wm_protocols_;
    ev.format = 32;
    ev.data.data32[0] = wm_take_focus_;
    ev.data.data32[1] = timestamp;

    xcb_send_event(conn_.get(), 0, client.id, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
}

bool WindowManager::cycle_focus(bool forward)
{
    if (focused_monitor_ >= monitors_.size())
        return false;

    auto& monitor = focused_monitor();
    auto& ws = monitor.current();

    auto eligible = [this](xcb_window_t window) { return is_focus_candidate(window); };

    auto floating_candidates = build_floating_candidates();

    auto get_mru = [this](xcb_window_t w) -> uint64_t
    {
        auto const* c = get_client(w);
        return c ? c->mru_order : 0;
    };

    auto candidates = focus_policy::build_cycle_candidates(
        ws.windows,
        floating_candidates,
        focused_monitor_,
        monitor.current_workspace,
        eligible,
        get_mru
    );

    auto target = forward ? focus_policy::cycle_focus_next(candidates, active_window_)
                          : focus_policy::cycle_focus_prev(candidates, active_window_);
    if (!target)
        return false;

    focus_any_window(target->id);
    return true;
}

std::vector<focus_policy::FloatingCandidate> WindowManager::build_floating_candidates() const
{
    struct Entry
    {
        focus_policy::FloatingCandidate cand;
        uint64_t mru;
    };
    std::vector<Entry> entries;
    entries.reserve(clients_.size());
    for (auto const& [window, client] : clients_)
    {
        if (client.kind() != Client::Kind::Floating)
            continue;
        entries.push_back({
            { window, client.monitor, client.workspace, client.sticky },
            client.mru_order
        });
    }
    std::sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) { return a.mru < b.mru; });

    std::vector<focus_policy::FloatingCandidate> candidates;
    candidates.reserve(entries.size());
    for (auto const& e : entries) candidates.push_back(e.cand);
    return candidates;
}

} // namespace lwm
