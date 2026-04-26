#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <algorithm>
#include <xcb/xcb_icccm.h>

namespace lwm {

void WindowManager::focus_any_window(xcb_window_t window, bool record_user_time)
{
    LOG_TRACE(
        "focus_any_window({:#x}) called, active_window_={:#x}, showing_desktop_={}",
        window,
        active_window_,
        showing_desktop_
    );

    if (showing_desktop_)
    {
        LOG_TRACE("focus_any_window: rejected (showing_desktop_)");
        return;
    }

    auto* client = get_client(window);
    if (!client)
    {
        LOG_TRACE("focus_any_window: rejected (no client)");
        return;
    }

    if (!is_focus_eligible(*client))
    {
        LOG_TRACE("focus_any_window: rejected (not focus eligible)");
        return;
    }

    bool is_floating = (client->kind == Client::Kind::Floating);

    if (client->iconic)
    {
        LOG_DEBUG("focus_any_window: deiconifying window {:#x}", window);
        deiconify_window(window, false);
    }

    xcb_window_t previous_active = active_window_;
    std::optional<size_t> previous_monitor;
    if (auto const* previous = get_client(previous_active))
        previous_monitor = previous->monitor;
    bool is_sticky = client->sticky;

    if (is_floating)
    {
        // Floating path: workspace switch + MRU promotion
        LOG_TRACE(
            "focus_any_window: floating path, client->monitor={} client->workspace={} "
            "current_workspace={} is_sticky={}",
            client->monitor,
            client->workspace,
            monitors_[client->monitor].current_workspace,
            is_sticky
        );

        focused_monitor_ = client->monitor;
        auto& monitor = monitors_[client->monitor];
        if (!is_sticky)
        {
            if (auto ctx = WorkspaceSwitchContext::validate(client->monitor, monitor, client->workspace))
            {
                LOG_DEBUG(
                    "focus_any_window({:#x}): WORKSPACE SWITCH TRIGGERED by focus! "
                    "old_ws={} new_ws={}",
                    window, ctx->old_workspace(), ctx->new_workspace()
                );
                perform_workspace_switch(*ctx);
                if (is_suppressed_by_fullscreen(*client))
                {
                    focus_or_fallback(monitors_[client->monitor], false);
                    return;
                }
            }
        }

        client->mru_order = next_mru_order_++;
        active_window_ = window;
    }
    else
    {
#ifndef NDEBUG
        auto const& recorded_workspace = monitors_[client->monitor].workspaces[client->workspace];
        if (recorded_workspace.find_window(window) == recorded_workspace.windows.end())
        {
            LOG_ERROR(
                "focus_any_window: tiled client {:#x} recorded at monitor={} workspace={} but is not in workspace",
                window,
                client->monitor,
                client->workspace
            );
        }
#endif

        LOG_DEBUG(
            "focus_any_window({:#x}): target_monitor={} workspace_changed={} "
            "old_ws={} new_ws={} prev_active={:#x}",
            window,
            client->monitor,
            !is_sticky && monitors_[client->monitor].current_workspace != client->workspace,
            monitors_[client->monitor].current_workspace,
            client->workspace,
            previous_active
        );

        focused_monitor_ = client->monitor;
        workspace_policy::set_workspace_focus(monitors_[client->monitor].workspaces[client->workspace], window);
        client->mru_order = next_mru_order_++;
        active_window_ = window;

        auto& monitor = monitors_[client->monitor];
        if (!is_sticky)
        {
            if (auto ctx = WorkspaceSwitchContext::validate(client->monitor, monitor, client->workspace))
            {
                LOG_DEBUG(
                    "focus_any_window: WORKSPACE SWITCH TRIGGERED by focus! old_ws={} new_ws={}",
                    ctx->old_workspace(), ctx->new_workspace()
                );
                perform_workspace_switch(*ctx);
                // After the workspace switch a sticky fullscreen owner may now suppress the target.
                if (is_suppressed_by_fullscreen(*client))
                {
                    active_window_ = previous_active;
                    focus_or_fallback(monitors_[client->monitor], false);
                    return;
                }
            }
        }
    }

    client = get_client(window);
    if (!client)
    {
        LOG_TRACE("focus_any_window: target disappeared before finalization");
        return;
    }

    if (active_window_ != window)
    {
        LOG_TRACE(
            "focus_any_window: focus redirected to {:#x}, skipping stale finalization for {:#x}",
            active_window_,
            window
        );
        return;
    }

    LOG_TRACE("focus_any_window: updating EWMH current desktop");
    update_ewmh_current_desktop();

    if (previous_active != XCB_NONE && previous_active != window && is_managed(previous_active))
    {
        if (auto const* prev_client = get_client(previous_active))
        {
            uint32_t color = border_color_for_client(*prev_client);
            xcb_change_window_attributes(conn_.get(), previous_active, XCB_CW_BORDER_PIXEL, &color);
        }
    }

    if (should_apply_focus_border(*client))
    {
        uint32_t focus_color = border_color_for_client(*client);
        xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &focus_color);
        uint32_t border_width = border_width_for_client(*client);
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);
        LOG_TRACE("focus_any_window: applied focus border visuals");
    }
    else
    {
        LOG_TRACE("focus_any_window: skipped focus border visuals (fullscreen/borderless)");
    }

    xcb_timestamp_t focus_time = last_event_time_ ? last_event_time_ : XCB_CURRENT_TIME;
    send_wm_take_focus(*client, focus_time);
    // Always set input focus directly on the target window.  ICCCM prescribes
    // root-focus for "Globally Active" windows (accepts_input=false), but doing
    // so leaves keyboard input stranded on root until the client responds to
    // WM_TAKE_FOCUS.  Following dwm/i3 convention we always focus the window;
    // a well-behaved Globally Active client can still redirect via WM_TAKE_FOCUS.
    xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, window, focus_time);

    if (previous_monitor && *previous_monitor != client->monitor)
        restack_monitor_layers(*previous_monitor);
    restack_monitor_layers(client->monitor);

    if (client->urgency.active())
        clear_client_urgency(*client);
    ewmh_.set_active_window(window);
    if (net_wm_state_focused_ != XCB_NONE)
    {
        if (previous_active != XCB_NONE && previous_active != window && is_managed(previous_active))
        {
            ewmh_.set_window_state(previous_active, net_wm_state_focused_, false);
        }
        ewmh_.set_window_state(window, net_wm_state_focused_, true);
    }

    if (record_user_time)
        client->user_time = last_event_time_;

    restack_transients(window);

    conn_.flush();

    emit_event(Event_FocusChange,
        "{\"event\":\"focus_change\",\"window\":" + std::to_string(window)
        + ",\"class\":\"" + json_escape(client->wm_class)
        + "\",\"title\":\"" + json_escape(client->name) + "\"}");

    LOG_TRACE("focus_any_window({:#x}): DONE", window);
}

void WindowManager::clear_focus()
{
    xcb_window_t previous_active = active_window_;
    std::optional<size_t> previous_monitor;
    if (auto const* previous = get_client(previous_active))
        previous_monitor = previous->monitor;
    active_window_ = XCB_NONE;
    ewmh_.set_active_window(XCB_NONE);
    xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, XCB_CURRENT_TIME);

    // Keep _NET_WM_STATE_FOCUSED in sync when focus is explicitly cleared.
    if (net_wm_state_focused_ != XCB_NONE && previous_active != XCB_NONE)
    {
        ewmh_.set_window_state(previous_active, net_wm_state_focused_, false);
    }

    if (previous_active != XCB_NONE && is_managed(previous_active))
    {
        if (auto const* prev_client = get_client(previous_active))
        {
            uint32_t color = border_color_for_client(*prev_client);
            xcb_change_window_attributes(conn_.get(), previous_active, XCB_CW_BORDER_PIXEL, &color);
        }
    }
    if (previous_monitor)
        restack_monitor_layers(*previous_monitor);

    conn_.flush();
}

void WindowManager::focus_or_fallback(Monitor& monitor, bool record_user_time)
{
    auto& ws = monitor.current();

    size_t monitor_idx = monitor_index(monitor);

    LOG_DEBUG(
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
        LOG_DEBUG("focus_or_fallback: invalid monitor, clearing focus");
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

    LOG_TRACE(
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
        LOG_DEBUG("focus_or_fallback: no candidate found, clearing focus");
        clear_focus();
        return;
    }

    LOG_DEBUG("focus_or_fallback: selected window={:#x} is_floating={}", selection->window, selection->is_floating);

    focus_any_window(selection->window, record_user_time);

    LOG_TRACE("focus_or_fallback: DONE");
}

bool WindowManager::is_focus_eligible(Client const& client) const
{
    if (client.kind == Client::Kind::Dock || client.kind == Client::Kind::Desktop)
        return false;
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

void WindowManager::cycle_focus(bool forward)
{
    if (monitors_.empty())
        return;

    auto& monitor = focused_monitor();
    auto& ws = monitor.current();

    auto eligible = [this](xcb_window_t window) { return is_focus_candidate(window); };

    auto floating_candidates = build_floating_candidates();

    auto get_mru = [this](xcb_window_t w) -> uint64_t {
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
        return;

    focus_any_window(target->id);
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
        if (client.kind != Client::Kind::Floating)
            continue;
        entries.push_back({ { window, client.monitor, client.workspace, client.sticky }, client.mru_order });
    }
    std::sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) { return a.mru < b.mru; });

    std::vector<focus_policy::FloatingCandidate> candidates;
    candidates.reserve(entries.size());
    for (auto const& e : entries)
        candidates.push_back(e.cand);
    return candidates;
}

} // namespace lwm
