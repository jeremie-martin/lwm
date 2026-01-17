#include "wm.hpp"
#include "lwm/core/log.hpp"
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/wait.h>
#include <unistd.h>

namespace lwm {

namespace {

void sigchld_handler(int /*sig*/)
{
    // Reap all zombie children
    while (waitpid(-1, nullptr, WNOHANG) > 0);
}

void setup_signal_handlers()
{
    struct sigaction sa = {};
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);
}

} // namespace

WindowManager::WindowManager(Config config)
    : config_(std::move(config))
    , conn_()
    , ewmh_(conn_)
    , keybinds_(conn_, config_)
    , layout_(conn_, config_.appearance)
    , bar_(
          config_.appearance.enable_internal_bar ? std::optional<StatusBar>(std::in_place, conn_, config_.appearance)
                                                 : std::nullopt
      )
{
    setup_signal_handlers();
    setup_root();
    detect_monitors();
    setup_ewmh();
    if (bar_)
    {
        setup_monitor_bars();
    }
    keybinds_.grab_keys(conn_.screen()->root);
    update_all_bars();
    conn_.flush();
}

void WindowManager::run()
{
    while (auto event = xcb_wait_for_event(conn_.get()))
    {
        std::unique_ptr<xcb_generic_event_t, decltype(&free)> eventPtr(event, free);
        handle_event(*eventPtr);
    }
}

void WindowManager::setup_root()
{
    uint32_t values[] = { XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                          | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_STRUCTURE_NOTIFY
                          | XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(conn_.get(), conn_.screen()->root, XCB_CW_EVENT_MASK, values);

    if (conn_.has_randr())
    {
        xcb_randr_select_input(conn_.get(), conn_.screen()->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }
}

void WindowManager::detect_monitors()
{
    monitors_.clear();

    if (!conn_.has_randr())
    {
        create_fallback_monitor();
        return;
    }

    auto res_cookie = xcb_randr_get_screen_resources_current(conn_.get(), conn_.screen()->root);
    auto* res_reply = xcb_randr_get_screen_resources_current_reply(conn_.get(), res_cookie, nullptr);

    if (!res_reply)
    {
        create_fallback_monitor();
        return;
    }

    int num_outputs = xcb_randr_get_screen_resources_current_outputs_length(res_reply);
    xcb_randr_output_t* outputs = xcb_randr_get_screen_resources_current_outputs(res_reply);

    for (int i = 0; i < num_outputs; ++i)
    {
        auto out_cookie = xcb_randr_get_output_info(conn_.get(), outputs[i], res_reply->config_timestamp);
        auto* out_reply = xcb_randr_get_output_info_reply(conn_.get(), out_cookie, nullptr);

        if (!out_reply)
            continue;
        if (out_reply->connection != XCB_RANDR_CONNECTION_CONNECTED || out_reply->crtc == XCB_NONE)
        {
            free(out_reply);
            continue;
        }

        int name_len = xcb_randr_get_output_info_name_length(out_reply);
        uint8_t* name_data = xcb_randr_get_output_info_name(out_reply);
        std::string output_name(reinterpret_cast<char*>(name_data), name_len);

        auto crtc_cookie = xcb_randr_get_crtc_info(conn_.get(), out_reply->crtc, res_reply->config_timestamp);
        auto* crtc_reply = xcb_randr_get_crtc_info_reply(conn_.get(), crtc_cookie, nullptr);

        if (crtc_reply && crtc_reply->width > 0 && crtc_reply->height > 0)
        {
            Monitor monitor;
            monitor.output = outputs[i];
            monitor.name = output_name;
            monitor.x = crtc_reply->x;
            monitor.y = crtc_reply->y;
            monitor.width = crtc_reply->width;
            monitor.height = crtc_reply->height;
            monitors_.push_back(monitor);
        }

        free(crtc_reply);
        free(out_reply);
    }

    free(res_reply);

    if (monitors_.empty())
    {
        create_fallback_monitor();
        return;
    }

    std::ranges::sort(monitors_, [](Monitor const& a, Monitor const& b) { return a.x < b.x; });
}

void WindowManager::create_fallback_monitor()
{
    Monitor monitor;
    monitor.name = "default";
    monitor.x = 0;
    monitor.y = 0;
    monitor.width = conn_.screen()->width_in_pixels;
    monitor.height = conn_.screen()->height_in_pixels;
    monitors_.push_back(monitor);
}

void WindowManager::setup_monitor_bars()
{
    if (!bar_)
        return;

    for (auto& monitor : monitors_)
    {
        if (monitor.bar_window != XCB_NONE)
        {
            bar_->destroy(monitor.bar_window);
        }
        monitor.bar_window = bar_->create_for_monitor(monitor);
    }
}

void WindowManager::handle_event(xcb_generic_event_t const& event)
{
    uint8_t response_type = event.response_type & ~0x80;

    if (conn_.has_randr() && response_type == conn_.randr_event_base() + XCB_RANDR_SCREEN_CHANGE_NOTIFY)
    {
        handle_randr_screen_change();
        return;
    }

    switch (response_type)
    {
        case XCB_MAP_REQUEST:
            handle_map_request(reinterpret_cast<xcb_map_request_event_t const&>(event));
            break;
        case XCB_UNMAP_NOTIFY:
        {
            auto const& e = reinterpret_cast<xcb_unmap_notify_event_t const&>(event);
            // Ignore if WM initiated this unmap (workspace switch)
            if (wm_unmapped_windows_.contains(e.window))
                break;
            // Client-initiated unmap - remove the window
            handle_window_removal(e.window);
            break;
        }
        case XCB_DESTROY_NOTIFY:
        {
            auto const& e = reinterpret_cast<xcb_destroy_notify_event_t const&>(event);
            handle_window_removal(e.window);
            wm_unmapped_windows_.erase(e.window); // Clean up tracking
            break;
        }
        case XCB_ENTER_NOTIFY:
            handle_enter_notify(reinterpret_cast<xcb_enter_notify_event_t const&>(event));
            break;
        case XCB_KEY_PRESS:
            handle_key_press(reinterpret_cast<xcb_key_press_event_t const&>(event));
            break;
        case XCB_CLIENT_MESSAGE:
            handle_client_message(reinterpret_cast<xcb_client_message_event_t const&>(event));
            break;
    }
}

void WindowManager::handle_map_request(xcb_map_request_event_t const& e)
{
    // Check if this is a dock window (e.g., Polybar)
    if (ewmh_.is_dock_window(e.window))
    {
        // Map but don't manage - let it float above
        xcb_map_window(conn_.get(), e.window);
        dock_windows_.push_back(e.window);
        update_struts();
        rearrange_all_monitors();
        conn_.flush();
        return;
    }

    // Check if window should not be tiled (dialogs, menus, etc.)
    if (!ewmh_.should_tile_window(e.window))
    {
        // Map but don't tile - let it float
        xcb_map_window(conn_.get(), e.window);
        conn_.flush();
        return;
    }

    manage_window(e.window);
    xcb_map_window(conn_.get(), e.window);
    focus_window(e.window);
}

void WindowManager::handle_window_removal(xcb_window_t window)
{
    unmanage_dock_window(window);
    unmanage_window(window);
}

void WindowManager::handle_enter_notify(xcb_enter_notify_event_t const& e)
{
    // Ignore internal bar windows
    for (auto const& monitor : monitors_)
    {
        if (e.event == monitor.bar_window)
            return;
    }

    // Ignore dock windows (e.g., Polybar)
    if (std::ranges::find(dock_windows_, e.event) != dock_windows_.end())
        return;

    if (e.event != conn_.screen()->root && e.mode == XCB_NOTIFY_MODE_NORMAL)
    {
        focus_window(e.event);
    }
}

void WindowManager::handle_key_press(xcb_key_press_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_press_event_t*>(&e), 0);

    LWM_DEBUG_KEY(e.state, keysym);

    auto action = keybinds_.resolve(e.state, keysym);
    if (!action)
    {
        LWM_DEBUG("No action for keysym");
        return;
    }

    LWM_DEBUG("Action: " << action->type);

    if (action->type == "kill" && focused_monitor().current().focused_window != XCB_NONE)
    {
        kill_window(focused_monitor().current().focused_window);
    }
    else if (action->type == "switch_workspace" && action->workspace >= 0)
    {
        switch_workspace(action->workspace);
    }
    else if (action->type == "move_to_workspace" && action->workspace >= 0)
    {
        move_window_to_workspace(action->workspace);
    }
    else if (action->type == "focus_monitor_left")
    {
        focus_monitor(-1);
    }
    else if (action->type == "focus_monitor_right")
    {
        focus_monitor(1);
    }
    else if (action->type == "move_to_monitor_left")
    {
        move_window_to_monitor(-1);
    }
    else if (action->type == "move_to_monitor_right")
    {
        move_window_to_monitor(1);
    }
    else if (action->type == "spawn")
    {
        launch_program(keybinds_.resolve_command(action->command, config_));
    }
}

void WindowManager::handle_client_message(xcb_client_message_event_t const& e)
{
    xcb_ewmh_connection_t* ewmh = ewmh_.get();

    // Handle _NET_CURRENT_DESKTOP requests (e.g., from Polybar clicks)
    if (e.type == ewmh->_NET_CURRENT_DESKTOP)
    {
        uint32_t desktop = e.data.data32[0];
        LWM_DEBUG("_NET_CURRENT_DESKTOP request: desktop=" << desktop);
        switch_to_ewmh_desktop(desktop);
    }
    // Handle _NET_ACTIVE_WINDOW requests
    else if (e.type == ewmh->_NET_ACTIVE_WINDOW)
    {
        xcb_window_t window = e.window;
        LWM_DEBUG("_NET_ACTIVE_WINDOW request: window=0x" << std::hex << window << std::dec);
        if (monitor_containing_window(window))
        {
            focus_window(window);
        }
    }
}

void WindowManager::handle_randr_screen_change()
{
    std::vector<Window> all_windows;
    for (auto& monitor : monitors_)
    {
        for (auto& workspace : monitor.workspaces)
        {
            for (auto& window : workspace.windows)
            {
                all_windows.push_back(window);
            }
        }
    }

    // Destroy old bar windows before detecting new monitors
    if (bar_)
    {
        for (auto const& monitor : monitors_)
        {
            if (monitor.bar_window != XCB_NONE)
            {
                bar_->destroy(monitor.bar_window);
            }
        }
    }

    detect_monitors();
    if (bar_)
    {
        setup_monitor_bars();
    }

    if (!monitors_.empty())
    {
        // Move all windows to first monitor, current workspace
        uint32_t desktop = get_ewmh_desktop_index(0, monitors_[0].current_workspace);
        for (auto& window : all_windows)
        {
            monitors_[0].current().windows.push_back(window);
            ewmh_.set_window_desktop(window.id, desktop);
        }
    }

    // Update EWMH for new monitor configuration
    ewmh_.set_desktop_viewport(monitors_);
    update_ewmh_client_list();

    focused_monitor_ = 0;
    update_ewmh_current_desktop();
    rearrange_all_monitors();

    // Focus a window after reconfiguration (Bug fix: was leaving focus unset)
    if (!monitors_.empty())
    {
        focus_or_fallback(monitors_[0]);
    }

    update_all_bars();
    conn_.flush();
}

void WindowManager::manage_window(xcb_window_t window)
{
    Window newWindow = { window, get_window_name(window) };
    focused_monitor().current().windows.push_back(newWindow);

    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
                          | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);

    // Set EWMH desktop for this window
    uint32_t desktop = get_ewmh_desktop_index(focused_monitor_, focused_monitor().current_workspace);
    ewmh_.set_window_desktop(window, desktop);
    update_ewmh_client_list();

    keybinds_.grab_keys(window);
    rearrange_monitor(focused_monitor());
    update_all_bars();
}

void WindowManager::unmanage_window(xcb_window_t window)
{
    for (auto& monitor : monitors_)
    {
        for (size_t ws_idx = 0; ws_idx < monitor.workspaces.size(); ++ws_idx)
        {
            auto& workspace = monitor.workspaces[ws_idx];
            auto it = workspace.find_window(window);
            if (it != workspace.windows.end())
            {
                workspace.windows.erase(it);
                bool was_focused = (workspace.focused_window == window);
                if (was_focused)
                {
                    workspace.focused_window = workspace.windows.empty() ? XCB_NONE : workspace.windows.back().id;
                }
                update_ewmh_client_list();
                rearrange_monitor(monitor);

                // If this was the focused window on the current workspace, update focus and EWMH
                if (was_focused && ws_idx == monitor.current_workspace)
                {
                    if (workspace.focused_window != XCB_NONE)
                    {
                        focus_window(workspace.focused_window);
                    }
                    else
                    {
                        ewmh_.set_active_window(XCB_NONE);
                    }
                }

                update_all_bars();
                conn_.flush();
                return;
            }
        }
    }
}

void WindowManager::focus_window(xcb_window_t window)
{
    // Find which monitor and workspace contains the window
    Monitor* target_monitor = nullptr;
    size_t target_monitor_idx = 0;
    size_t target_workspace_idx = 0;

    for (size_t m = 0; m < monitors_.size(); ++m)
    {
        for (size_t w = 0; w < monitors_[m].workspaces.size(); ++w)
        {
            if (monitors_[m].workspaces[w].find_window(window) != monitors_[m].workspaces[w].windows.end())
            {
                target_monitor = &monitors_[m];
                target_monitor_idx = m;
                target_workspace_idx = w;
                break;
            }
        }
        if (target_monitor)
            break;
    }

    if (!target_monitor)
        return;

    // Switch to target monitor
    focused_monitor_ = target_monitor_idx;

    // If window is on a different workspace, switch to it
    if (target_monitor->current_workspace != target_workspace_idx)
    {
        // Unmap current workspace windows
        for (auto const& w : target_monitor->current().windows)
        {
            wm_unmap_window(w.id);
        }
        target_monitor->current_workspace = target_workspace_idx;
        update_ewmh_current_desktop();
        rearrange_monitor(*target_monitor);
    }

    // Clear borders on all windows across all monitors
    clear_all_borders();

    auto& workspace = target_monitor->current();
    workspace.focused_window = window;
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &config_.appearance.border_color);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);
    xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME);

    // Clear urgent hint when window receives focus
    ewmh_.set_demands_attention(window, false);
    ewmh_.set_active_window(window);

    update_all_bars();
    conn_.flush();
}

void WindowManager::kill_window(xcb_window_t window)
{
    // Per ICCCM, WM_DELETE_WINDOW must be sent as ClientMessage with type=WM_PROTOCOLS
    auto protocols_cookie = xcb_intern_atom(conn_.get(), 0, 12, "WM_PROTOCOLS");
    auto delete_cookie = xcb_intern_atom(conn_.get(), 0, 16, "WM_DELETE_WINDOW");
    auto* protocols_reply = xcb_intern_atom_reply(conn_.get(), protocols_cookie, nullptr);
    auto* delete_reply = xcb_intern_atom_reply(conn_.get(), delete_cookie, nullptr);

    if (protocols_reply && delete_reply)
    {
        xcb_client_message_event_t ev = {};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = window;
        ev.type = protocols_reply->atom; // Must be WM_PROTOCOLS per ICCCM
        ev.format = 32;
        ev.data.data32[0] = delete_reply->atom; // WM_DELETE_WINDOW
        ev.data.data32[1] = XCB_CURRENT_TIME;

        xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
        conn_.flush(); // Flush before sleeping to ensure message is sent
    }

    free(protocols_reply);
    free(delete_reply);

    usleep(100000);
    xcb_kill_client(conn_.get(), window);
    unmanage_window(window);
    conn_.flush();
}

void WindowManager::rearrange_monitor(Monitor& monitor)
{
    // Only arrange current workspace - callers handle hiding old workspace
    layout_.arrange(monitor.current().windows, monitor.working_area(), bar_.has_value());

    // Clear unmap tracking for windows that are now visible
    for (auto const& window : monitor.current().windows)
    {
        wm_unmapped_windows_.erase(window.id);
    }
}

void WindowManager::rearrange_all_monitors()
{
    for (auto& monitor : monitors_)
    {
        rearrange_monitor(monitor);
    }
}

void WindowManager::switch_workspace(int ws)
{
    if (ws < 0 || ws >= Config::NUM_WORKSPACES || static_cast<size_t>(ws) == focused_monitor().current_workspace)
        return;

    auto& monitor = focused_monitor();

    for (auto const& window : monitor.current().windows)
    {
        wm_unmap_window(window.id);
    }

    monitor.current_workspace = static_cast<size_t>(ws);
    update_ewmh_current_desktop();
    rearrange_monitor(monitor);
    focus_or_fallback(monitor);
    update_all_bars();
    conn_.flush();
}

void WindowManager::move_window_to_workspace(int ws)
{
    auto& monitor = focused_monitor();
    auto& current_ws = monitor.current();

    if (ws < 0 || ws >= Config::NUM_WORKSPACES || static_cast<size_t>(ws) == monitor.current_workspace)
        return;

    if (current_ws.focused_window == XCB_NONE)
        return;

    xcb_window_t window_to_move = current_ws.focused_window;

    auto it = current_ws.find_window(window_to_move);
    if (it == current_ws.windows.end())
        return;

    Window moved_window = *it;
    current_ws.windows.erase(it);
    current_ws.focused_window = XCB_NONE;

    monitor.workspaces[ws].windows.push_back(moved_window);
    monitor.workspaces[ws].focused_window = window_to_move;

    // Update EWMH desktop for moved window
    uint32_t desktop = get_ewmh_desktop_index(focused_monitor_, static_cast<size_t>(ws));
    ewmh_.set_window_desktop(window_to_move, desktop);

    wm_unmap_window(window_to_move);
    rearrange_monitor(monitor);

    if (!current_ws.windows.empty())
    {
        focus_window(current_ws.windows.back().id);
    }
    else
    {
        // Workspace is now empty - update EWMH to reflect no active window
        ewmh_.set_active_window(XCB_NONE);
    }

    update_all_bars();
    conn_.flush();
}

size_t WindowManager::wrap_monitor_index(int idx) const
{
    int size = static_cast<int>(monitors_.size());
    return static_cast<size_t>(((idx % size) + size) % size);
}

void WindowManager::warp_to_monitor(Monitor const& monitor)
{
    xcb_warp_pointer(
        conn_.get(),
        XCB_NONE,
        conn_.screen()->root,
        0,
        0,
        0,
        0,
        monitor.x + monitor.width / 2,
        monitor.y + monitor.height / 2
    );
}

void WindowManager::focus_or_fallback(Monitor& monitor)
{
    auto& ws = monitor.current();

    // Verify focused_window actually exists in the workspace (defensive programming)
    if (ws.focused_window != XCB_NONE && ws.find_window(ws.focused_window) != ws.windows.end())
    {
        focus_window(ws.focused_window);
    }
    else if (!ws.windows.empty())
    {
        // focused_window was stale or XCB_NONE - focus last window
        focus_window(ws.windows.back().id);
    }
    else
    {
        // No windows - set active window to none per EWMH spec
        ewmh_.set_active_window(XCB_NONE);
    }
}

void WindowManager::focus_monitor(int direction)
{
    if (monitors_.size() <= 1)
        return;

    focused_monitor_ = wrap_monitor_index(static_cast<int>(focused_monitor_) + direction);
    update_ewmh_current_desktop();

    auto& monitor = focused_monitor();
    focus_or_fallback(monitor);
    warp_to_monitor(monitor);

    update_all_bars();
    conn_.flush();
}

void WindowManager::move_window_to_monitor(int direction)
{
    if (monitors_.size() <= 1)
        return;

    auto& source_ws = focused_monitor().current();
    if (source_ws.focused_window == XCB_NONE)
        return;

    size_t target_idx = wrap_monitor_index(static_cast<int>(focused_monitor_) + direction);
    if (target_idx == focused_monitor_)
        return;

    xcb_window_t window_to_move = source_ws.focused_window;

    auto it = source_ws.find_window(window_to_move);
    if (it == source_ws.windows.end())
        return;

    Window moved_window = *it;
    source_ws.windows.erase(it);

    // Update source workspace's focused_window to another window if any remain
    if (!source_ws.windows.empty())
    {
        source_ws.focused_window = source_ws.windows.back().id;
    }
    else
    {
        source_ws.focused_window = XCB_NONE;
    }

    auto& target_monitor = monitors_[target_idx];
    target_monitor.current().windows.push_back(moved_window);
    target_monitor.current().focused_window = window_to_move;

    // Update EWMH desktop for moved window
    uint32_t desktop = get_ewmh_desktop_index(target_idx, target_monitor.current_workspace);
    ewmh_.set_window_desktop(window_to_move, desktop);

    rearrange_monitor(focused_monitor());
    rearrange_monitor(target_monitor);

    focused_monitor_ = target_idx;
    update_ewmh_current_desktop();
    focus_window(window_to_move);
    warp_to_monitor(target_monitor);

    update_all_bars();
    conn_.flush();
}

void WindowManager::launch_program(std::string const& command)
{
    if (fork() == 0)
    {
        setsid();
        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        exit(1);
    }
}

Monitor* WindowManager::monitor_containing_window(xcb_window_t window)
{
    // Search ALL workspaces on ALL monitors, not just current workspace
    for (auto& monitor : monitors_)
    {
        for (auto& workspace : monitor.workspaces)
        {
            if (workspace.find_window(window) != workspace.windows.end())
            {
                return &monitor;
            }
        }
    }
    return nullptr;
}

Monitor* WindowManager::monitor_at_point(int16_t x, int16_t y)
{
    for (auto& monitor : monitors_)
    {
        if (x >= monitor.x && x < monitor.x + monitor.width && y >= monitor.y && y < monitor.y + monitor.height)
        {
            return &monitor;
        }
    }
    return monitors_.empty() ? nullptr : &monitors_[0];
}

std::string WindowManager::get_window_name(xcb_window_t window)
{
    auto cookie = xcb_get_property(conn_.get(), 0, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 1024);
    auto* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);

    if (reply)
    {
        int len = xcb_get_property_value_length(reply);
        char* name = static_cast<char*>(xcb_get_property_value(reply));
        std::string windowName(name, len);
        free(reply);
        return windowName;
    }
    return "Unnamed";
}

void WindowManager::update_all_bars()
{
    if (!bar_)
        return;

    for (auto const& monitor : monitors_)
    {
        bar_->update(monitor);
    }
}

void WindowManager::update_struts()
{
    // Reset all monitor struts
    for (auto& monitor : monitors_)
    {
        monitor.strut = {};
    }

    // Query struts from all dock windows and apply to appropriate monitor
    for (xcb_window_t dock : dock_windows_)
    {
        Strut strut = ewmh_.get_window_strut(dock);
        if (strut.left == 0 && strut.right == 0 && strut.top == 0 && strut.bottom == 0)
            continue;

        // Get dock window geometry to determine which monitor it's on
        auto geom_cookie = xcb_get_geometry(conn_.get(), dock);
        auto* geom = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
        if (!geom)
            continue;

        // Find the monitor containing this dock
        Monitor* target = monitor_at_point(geom->x, geom->y);
        free(geom);

        if (target)
        {
            // Aggregate struts (take maximum for each side)
            target->strut.left = std::max(target->strut.left, strut.left);
            target->strut.right = std::max(target->strut.right, strut.right);
            target->strut.top = std::max(target->strut.top, strut.top);
            target->strut.bottom = std::max(target->strut.bottom, strut.bottom);
        }
    }
}

void WindowManager::unmanage_dock_window(xcb_window_t window)
{
    auto it = std::ranges::find(dock_windows_, window);
    if (it != dock_windows_.end())
    {
        dock_windows_.erase(it);
        update_struts();
        rearrange_all_monitors();
    }
}

void WindowManager::wm_unmap_window(xcb_window_t window)
{
    wm_unmapped_windows_.insert(window);
    xcb_unmap_window(conn_.get(), window);
}

void WindowManager::setup_ewmh()
{
    ewmh_.init_atoms();
    ewmh_.set_wm_name("lwm");

    // Total desktops = monitors * workspaces per monitor
    uint32_t total_desktops = static_cast<uint32_t>(monitors_.size() * Config::NUM_WORKSPACES);
    ewmh_.set_number_of_desktops(total_desktops);

    // Generate desktop names: "Mon0:1", "Mon0:2", ... "Mon1:1", etc.
    std::vector<std::string> names;
    for (size_t m = 0; m < monitors_.size(); ++m)
    {
        for (int w = 0; w < Config::NUM_WORKSPACES; ++w)
        {
            names.push_back(monitors_[m].name + ":" + std::to_string(w + 1));
        }
    }
    ewmh_.set_desktop_names(names);

    ewmh_.set_desktop_viewport(monitors_);
    update_ewmh_current_desktop();
}

void WindowManager::update_ewmh_client_list()
{
    std::vector<xcb_window_t> windows;
    for (auto const& monitor : monitors_)
    {
        for (auto const& workspace : monitor.workspaces)
        {
            for (auto const& window : workspace.windows)
            {
                windows.push_back(window.id);
            }
        }
    }
    ewmh_.update_client_list(windows);
}

void WindowManager::update_ewmh_current_desktop()
{
    uint32_t desktop = get_ewmh_desktop_index(focused_monitor_, focused_monitor().current_workspace);
    ewmh_.set_current_desktop(desktop);
}

uint32_t WindowManager::get_ewmh_desktop_index(size_t monitor_idx, size_t workspace_idx) const
{
    // Desktop index = monitor_idx * workspaces_per_monitor + workspace_idx
    return static_cast<uint32_t>(monitor_idx * Config::NUM_WORKSPACES + workspace_idx);
}

void WindowManager::switch_to_ewmh_desktop(uint32_t desktop)
{
    // Convert EWMH desktop index to monitor + workspace
    size_t monitor_idx = desktop / Config::NUM_WORKSPACES;
    size_t workspace_idx = desktop % Config::NUM_WORKSPACES;

    if (monitor_idx >= monitors_.size())
        return;

    auto& monitor = monitors_[monitor_idx];

    // Early return if already on target monitor and workspace (matches switch_workspace behavior)
    if (monitor_idx == focused_monitor_ && workspace_idx == monitor.current_workspace)
        return;

    // Unmap windows from OLD workspace before switching
    for (auto const& window : monitor.current().windows)
    {
        wm_unmap_window(window.id);
    }

    // Switch to target monitor and workspace
    focused_monitor_ = monitor_idx;
    monitor.current_workspace = workspace_idx;
    update_ewmh_current_desktop();
    rearrange_monitor(monitor);
    focus_or_fallback(monitor);
    update_all_bars();
    conn_.flush();
}

void WindowManager::clear_all_borders()
{
    for (auto& monitor : monitors_)
    {
        for (auto& workspace : monitor.workspaces)
        {
            for (auto const& window : workspace.windows)
            {
                xcb_change_window_attributes(
                    conn_.get(),
                    window.id,
                    XCB_CW_BORDER_PIXEL,
                    &conn_.screen()->black_pixel
                );
            }
        }
    }
    conn_.flush();
}

} // namespace lwm
