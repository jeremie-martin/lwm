#include "wm.hpp"
#include "lwm/core/floating.hpp"
#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include <algorithm>
#include <xcb/xcb_icccm.h>

namespace lwm {

namespace {

constexpr uint32_t WM_STATE_WITHDRAWN = 0;
constexpr uint32_t WM_STATE_NORMAL = 1;
constexpr uint32_t WM_STATE_ICONIC = 3;

} // namespace

void WindowManager::manage_floating_window(xcb_window_t window, bool start_iconic)
{
    auto transient = transient_for_window(window);
    std::optional<size_t> monitor_idx;
    std::optional<size_t> workspace_idx;
    std::optional<Geometry> parent_geom;

    if (transient)
    {
        monitor_idx = monitor_index_for_window(*transient);
        workspace_idx = workspace_index_for_window(*transient);

        auto geom_cookie = xcb_get_geometry(conn_.get(), *transient);
        auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
        if (geom_reply)
        {
            parent_geom = Geometry{ geom_reply->x, geom_reply->y, geom_reply->width, geom_reply->height };
            free(geom_reply);
        }
    }

    if (!monitor_idx || !workspace_idx)
    {
        if (auto target = resolve_window_desktop(window))
        {
            monitor_idx = target->first;
            workspace_idx = target->second;
        }
    }

    if (!monitor_idx)
        monitor_idx = focused_monitor_;
    if (!workspace_idx)
        workspace_idx = monitors_[*monitor_idx].current_workspace;

    xcb_size_hints_t size_hints;
    bool has_hints = xcb_icccm_get_wm_normal_hints_reply(
        conn_.get(),
        xcb_icccm_get_wm_normal_hints(conn_.get(), window),
        &size_hints,
        nullptr
    );
    bool has_position_hint = false;
    bool has_size_hint = false;
    int16_t hinted_x = 0;
    int16_t hinted_y = 0;
    uint32_t hinted_width = 0;
    uint32_t hinted_height = 0;
    if (has_hints)
    {
        if (size_hints.flags & (XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_P_POSITION))
        {
            has_position_hint = true;
            hinted_x = static_cast<int16_t>(size_hints.x);
            hinted_y = static_cast<int16_t>(size_hints.y);
        }
        if (size_hints.flags & (XCB_ICCCM_SIZE_HINT_US_SIZE | XCB_ICCCM_SIZE_HINT_P_SIZE))
        {
            has_size_hint = true;
            hinted_width = static_cast<uint32_t>(size_hints.width);
            hinted_height = static_cast<uint32_t>(size_hints.height);
        }
    }

    uint32_t width = 300;
    uint32_t height = 200;
    auto geom_cookie = xcb_get_geometry(conn_.get(), window);
    auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
    if (geom_reply)
    {
        width = geom_reply->width;
        height = geom_reply->height;
        free(geom_reply);
    }
    if (has_size_hint)
    {
        if (hinted_width > 0)
            width = hinted_width;
        if (hinted_height > 0)
            height = hinted_height;
    }
    if (width == 0)
        width = 300;
    if (height == 0)
        height = 200;
    layout_.apply_size_hints(window, width, height);

    Geometry placement;
    if (has_position_hint)
    {
        placement.x = hinted_x;
        placement.y = hinted_y;
        placement.width = static_cast<uint16_t>(width);
        placement.height = static_cast<uint16_t>(height);
    }
    else
    {
        placement = floating::place_floating(
            monitors_[*monitor_idx].working_area(),
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height),
            parent_geom
        );
    }

    FloatingWindow floating_window;
    floating_window.id = window;
    floating_window.geometry = placement;
    floating_windows_.push_back(floating_window);

    // Populate unified Client registry (authoritative for all non-geometry state)
    {
        auto [instance_name, class_name] = get_wm_class(window);
        Client client;
        client.id = window;
        client.kind = Client::Kind::Floating;
        client.monitor = *monitor_idx;
        client.workspace = *workspace_idx;
        client.name = get_window_name(window);
        client.wm_class = class_name;
        client.wm_class_name = instance_name;
        client.floating_geometry = placement;
        client.transient_for = transient.value_or(XCB_NONE);
        client.order = next_client_order_++;
        client.iconic = start_iconic;

        // Read and apply initial _NET_WM_STATE atoms (EWMH)
        xcb_ewmh_get_atoms_reply_t initial_state;
        if (xcb_ewmh_get_wm_state_reply(ewmh_.get(), xcb_ewmh_get_wm_state(ewmh_.get(), window), &initial_state, nullptr))
        {
            xcb_ewmh_connection_t* ewmh = ewmh_.get();
            for (uint32_t i = 0; i < initial_state.atoms_len; ++i)
            {
                xcb_atom_t state = initial_state.atoms[i];
                if (state == ewmh->_NET_WM_STATE_FULLSCREEN)
                    client.fullscreen = true;
                else if (state == ewmh->_NET_WM_STATE_ABOVE)
                    client.above = true;
                else if (state == ewmh->_NET_WM_STATE_BELOW)
                    client.below = true;
                else if (state == ewmh->_NET_WM_STATE_STICKY)
                    client.sticky = true;
                else if (state == ewmh->_NET_WM_STATE_MAXIMIZED_HORZ)
                    client.maximized_horz = true;
                else if (state == ewmh->_NET_WM_STATE_MAXIMIZED_VERT)
                    client.maximized_vert = true;
                else if (state == ewmh->_NET_WM_STATE_SHADED)
                    client.shaded = true;
                else if (state == ewmh->_NET_WM_STATE_MODAL)
                    client.modal = true;
                else if (state == ewmh->_NET_WM_STATE_SKIP_TASKBAR)
                    client.skip_taskbar = true;
                else if (state == ewmh->_NET_WM_STATE_SKIP_PAGER)
                    client.skip_pager = true;
                else if (state == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION)
                    client.demands_attention = true;
                else if (state == ewmh->_NET_WM_STATE_HIDDEN)
                    client.iconic = true;
            }
            xcb_ewmh_get_atoms_reply_wipe(&initial_state);
        }

        // Transients automatically get skip_taskbar/skip_pager (per COMPLIANCE.md)
        if (transient)
        {
            client.skip_taskbar = true;
            client.skip_pager = true;
        }

        clients_[window] = std::move(client);
    }

    // Read _NET_WM_USER_TIME_WINDOW if present (EWMH focus stealing prevention)
    if (net_wm_user_time_window_ != XCB_NONE)
    {
        auto cookie = xcb_get_property(conn_.get(), 0, window, net_wm_user_time_window_, XCB_ATOM_WINDOW, 0, 1);
        auto* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);
        if (reply)
        {
            if (xcb_get_property_value_length(reply) >= 4)
            {
                xcb_window_t time_window = *static_cast<xcb_window_t*>(xcb_get_property_value(reply));
                if (time_window != XCB_NONE)
                {
                    // User time window is authoritative in Client
                    if (auto* client = get_client(window))
                        client->user_time_window = time_window;
                    // Select PropertyNotify on the user time window to track changes
                    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
                    xcb_change_window_attributes(conn_.get(), time_window, XCB_CW_EVENT_MASK, &mask);
                }
            }
            free(reply);
        }
    }
    // User time is authoritative in Client
    if (auto* client = get_client(window))
        client->user_time = get_user_time(window);

    // Note: We intentionally do NOT select STRUCTURE_NOTIFY on client windows.
    // We receive UnmapNotify/DestroyNotify via root's SubstructureNotifyMask.
    // Selecting STRUCTURE_NOTIFY on clients would cause duplicate UnmapNotify events,
    // leading to incorrect unmanagement of windows during workspace switches (ICCCM compliance).
    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE
                          | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { start_iconic ? WM_STATE_ICONIC : WM_STATE_NORMAL, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    if (start_iconic)
    {
        // client->iconic is already set in the Client initialization above
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_HIDDEN, true);
    }

    update_sync_state(window);
    update_fullscreen_monitor_state(window);

    // Set EWMH properties
    ewmh_.set_frame_extents(window, 0, 0, 0, 0); // LWM doesn't add frames
    uint32_t desktop = get_ewmh_desktop_index(*monitor_idx, *workspace_idx);
    ewmh_.set_window_desktop(window, desktop);

    // Set allowed actions for floating windows (includes move/resize)
    xcb_ewmh_connection_t* ewmh = ewmh_.get();
    std::vector<xcb_atom_t> actions = {
        ewmh->_NET_WM_ACTION_CLOSE,  ewmh->_NET_WM_ACTION_FULLSCREEN, ewmh->_NET_WM_ACTION_CHANGE_DESKTOP,
        ewmh->_NET_WM_ACTION_ABOVE,  ewmh->_NET_WM_ACTION_BELOW,      ewmh->_NET_WM_ACTION_MINIMIZE,
        ewmh->_NET_WM_ACTION_SHADE,  ewmh->_NET_WM_ACTION_STICK,       ewmh->_NET_WM_ACTION_MAXIMIZE_VERT,
        ewmh->_NET_WM_ACTION_MAXIMIZE_HORZ,
        ewmh->_NET_WM_ACTION_MOVE,
        ewmh->_NET_WM_ACTION_RESIZE,
    };
    ewmh_.set_allowed_actions(window, actions);

    update_ewmh_client_list();

    keybinds_.grab_keys(window);

    // BEFORE mapping: Apply geometry-affecting states so window appears at correct position
    // (See COMPLIANCE.md "Window State Application Ordering")
    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN))
    {
        set_fullscreen(window, true);
    }

    bool wants_max_horz = ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_HORZ);
    bool wants_max_vert = ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_VERT);
    if (wants_max_horz || wants_max_vert)
    {
        set_window_maximized(window, wants_max_horz, wants_max_vert);
    }

    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_SHADED))
    {
        set_window_shaded(window, true);
    }

    // NOW map the window (with correct geometry already applied)
    if (!start_iconic)
    {
        update_floating_visibility(*monitor_idx);
        if (!suppress_focus_ && *monitor_idx == focused_monitor_ && is_workspace_visible(*monitor_idx, *workspace_idx))
            focus_floating_window(window);
    }

    // AFTER mapping: Apply non-geometry states
    // Transient windows should not appear in taskbars/pagers (ICCCM/EWMH convention)
    // Set SKIP_TASKBAR and SKIP_PAGER unless the window explicitly overrides this
    if (transient && !ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_TASKBAR))
    {
        set_client_skip_taskbar(window, true);
    }
    if (transient && !ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_PAGER))
    {
        set_client_skip_pager(window, true);
    }
    // Also honor explicit client requests
    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_TASKBAR))
    {
        set_client_skip_taskbar(window, true);
    }
    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_PAGER))
    {
        set_client_skip_pager(window, true);
    }

    // Honor _NET_WM_DESKTOP = 0xFFFFFFFF as sticky at manage time
    if (is_sticky_desktop(window))
    {
        set_window_sticky(window, true);
    }
    else if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_STICKY))
    {
        set_window_sticky(window, true);
    }

    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_MODAL))
    {
        set_window_modal(window, true);
    }

    bool wants_above = ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE);
    bool wants_below = ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_BELOW);
    if (wants_above)
    {
        set_window_above(window, true);
    }
    else if (wants_below)
    {
        set_window_below(window, true);
    }
}

void WindowManager::unmanage_floating_window(xcb_window_t window)
{
    // Set WM_STATE to Withdrawn before unmanaging (ICCCM)
    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_WITHDRAWN, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    // Tracking state that remains outside Client
    pending_kills_.erase(window);
    pending_pings_.erase(window);

    // Get monitor/workspace from Client before erasing
    size_t monitor_idx = 0;
    if (auto const* client = get_client(window))
    {
        monitor_idx = client->monitor;
    }

    // Remove from unified Client registry (handles all client state including order)
    clients_.erase(window);

    auto it = std::ranges::find_if(
        floating_windows_,
        [window](FloatingWindow const& fw) { return fw.id == window; }
    );
    if (it == floating_windows_.end())
        return;

    bool was_active = (active_window_ == window);
    floating_windows_.erase(it);
    update_ewmh_client_list();

    if (was_active)
    {
        if (monitor_idx == focused_monitor_ && monitor_idx < monitors_.size())
        {
            focus_or_fallback(monitors_[monitor_idx]);
        }
        else
        {
            clear_focus();
        }
    }

    update_all_bars();
    conn_.flush();
}

WindowManager::FloatingWindow* WindowManager::find_floating_window(xcb_window_t window)
{
    auto it = std::find_if(
        floating_windows_.begin(),
        floating_windows_.end(),
        [window](FloatingWindow const& floating_window) { return floating_window.id == window; }
    );
    return it == floating_windows_.end() ? nullptr : &(*it);
}

WindowManager::FloatingWindow const* WindowManager::find_floating_window(xcb_window_t window) const
{
    auto it = std::find_if(
        floating_windows_.begin(),
        floating_windows_.end(),
        [window](FloatingWindow const& floating_window) { return floating_window.id == window; }
    );
    return it == floating_windows_.end() ? nullptr : &(*it);
}

void WindowManager::update_floating_visibility(size_t monitor_idx)
{
    LOG_TRACE("update_floating_visibility({}) called, current_ws={}",
              monitor_idx, monitor_idx < monitors_.size() ? monitors_[monitor_idx].current_workspace : 0);

    if (monitor_idx >= monitors_.size())
    {
        LOG_TRACE("update_floating_visibility: invalid monitor index");
        return;
    }

    auto& monitor = monitors_[monitor_idx];
    if (showing_desktop_)
    {
        LOG_TRACE("update_floating_visibility: showing desktop, hiding all floating");
        for (auto& fw : floating_windows_)
        {
            auto const* client = get_client(fw.id);
            if (client && client->monitor == monitor_idx)
            {
                LOG_TRACE("update_floating_visibility: unmapping floating {:#x} (showing desktop)", fw.id);
                wm_unmap_window(fw.id);
            }
        }
        return;
    }

    for (auto& fw : floating_windows_)
    {
        auto const* client = get_client(fw.id);
        if (!client || client->monitor != monitor_idx)
            continue;

        bool sticky = is_client_sticky(fw.id);
        bool should_show = (sticky || client->workspace == monitor.current_workspace)
            && !is_client_iconic(fw.id);

        LOG_TRACE("update_floating_visibility: window {:#x} ws={} current_ws={} sticky={} iconic={} -> show={}",
                  fw.id, client->workspace, monitor.current_workspace, sticky,
                  is_client_iconic(fw.id), should_show);

        if (should_show)
        {
            // Configure BEFORE mapping so window appears at correct position
            if (is_client_fullscreen(fw.id))
            {
                apply_fullscreen_if_needed(fw.id);
            }
            else if (is_client_maximized_horz(fw.id)
                || is_client_maximized_vert(fw.id))
            {
                apply_maximized_geometry(fw.id);
            }
            else
            {
                apply_floating_geometry(fw);
            }
            LOG_TRACE("update_floating_visibility: mapping floating {:#x}", fw.id);
            xcb_map_window(conn_.get(), fw.id);
            if (client->transient_for != XCB_NONE)
            {
                restack_transients(client->transient_for);
            }
        }
        else
        {
            LOG_TRACE("update_floating_visibility: unmapping floating {:#x}", fw.id);
            wm_unmap_window(fw.id);
        }
    }
    LOG_TRACE("update_floating_visibility: DONE");
}

void WindowManager::update_floating_visibility_all()
{
    for (size_t i = 0; i < monitors_.size(); ++i)
    {
        update_floating_visibility(i);
    }
}

void WindowManager::update_floating_monitor_for_geometry(FloatingWindow& window)
{
    auto* client = get_client(window.id);
    if (!client)
        return;

    int32_t center_x = static_cast<int32_t>(window.geometry.x) + static_cast<int32_t>(window.geometry.width) / 2;
    int32_t center_y = static_cast<int32_t>(window.geometry.y) + static_cast<int32_t>(window.geometry.height) / 2;
    auto new_monitor =
        focus::monitor_index_at_point(monitors_, static_cast<int16_t>(center_x), static_cast<int16_t>(center_y));
    if (!new_monitor || *new_monitor == client->monitor)
        return;

    // Update Client (authoritative for monitor/workspace)
    client->monitor = *new_monitor;
    client->workspace = monitors_[client->monitor].current_workspace;

    uint32_t desktop = get_ewmh_desktop_index(client->monitor, client->workspace);
    ewmh_.set_window_desktop(window.id, desktop);
}

void WindowManager::apply_floating_geometry(FloatingWindow const& window)
{
    uint32_t width = window.geometry.width;
    uint32_t height = window.geometry.height;
    layout_.apply_size_hints(window.id, width, height);

    send_sync_request(window.id, last_event_time_);

    int32_t x = static_cast<int32_t>(window.geometry.x);
    int32_t y = static_cast<int32_t>(window.geometry.y);

    uint32_t values[] = { static_cast<uint32_t>(x), static_cast<uint32_t>(y), width, height };
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    xcb_configure_window(conn_.get(), window.id, mask, values);

    // Send synthetic ConfigureNotify so client knows its geometry immediately
    xcb_configure_notify_event_t ev = {};
    ev.response_type = XCB_CONFIGURE_NOTIFY;
    ev.event = window.id;
    ev.window = window.id;
    ev.x = static_cast<int16_t>(x);
    ev.y = static_cast<int16_t>(y);
    ev.width = static_cast<uint16_t>(width);
    ev.height = static_cast<uint16_t>(height);
    ev.border_width = static_cast<uint16_t>(config_.appearance.border_width);
    ev.above_sibling = XCB_NONE;
    ev.override_redirect = 0;

    xcb_send_event(conn_.get(), 0, window.id, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<char*>(&ev));
}

} // namespace lwm
