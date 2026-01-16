#include "wm.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <ranges>
#include <unistd.h>

namespace lwm {

WindowManager::WindowManager(Config config)
    : config_(std::move(config))
    , conn_()
    , keybinds_(conn_, config_)
    , layout_(conn_, config_.appearance)
    , bar_(conn_, config_.appearance)
{
    setup_root();
    detect_monitors();
    setup_monitor_bars();
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

    // Subscribe to RANDR screen change events if available
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
        std::cout << "RANDR not available, using fallback single monitor" << std::endl;
        create_fallback_monitor();
        return;
    }

    xcb_randr_get_screen_resources_current_cookie_t res_cookie =
        xcb_randr_get_screen_resources_current(conn_.get(), conn_.screen()->root);
    xcb_randr_get_screen_resources_current_reply_t* res_reply =
        xcb_randr_get_screen_resources_current_reply(conn_.get(), res_cookie, nullptr);

    if (!res_reply)
    {
        std::cout << "Failed to get screen resources, using fallback" << std::endl;
        create_fallback_monitor();
        return;
    }

    int num_outputs = xcb_randr_get_screen_resources_current_outputs_length(res_reply);
    xcb_randr_output_t* outputs = xcb_randr_get_screen_resources_current_outputs(res_reply);

    std::cout << "Found " << num_outputs << " outputs" << std::endl;

    for (int i = 0; i < num_outputs; ++i)
    {
        xcb_randr_get_output_info_cookie_t out_cookie =
            xcb_randr_get_output_info(conn_.get(), outputs[i], res_reply->config_timestamp);
        xcb_randr_get_output_info_reply_t* out_reply =
            xcb_randr_get_output_info_reply(conn_.get(), out_cookie, nullptr);

        if (!out_reply)
            continue;
        if (out_reply->connection != XCB_RANDR_CONNECTION_CONNECTED || out_reply->crtc == XCB_NONE)
        {
            free(out_reply);
            continue;
        }

        // Get output name
        int name_len = xcb_randr_get_output_info_name_length(out_reply);
        uint8_t* name_data = xcb_randr_get_output_info_name(out_reply);
        std::string output_name(reinterpret_cast<char*>(name_data), name_len);

        // Get CRTC info for geometry
        xcb_randr_get_crtc_info_cookie_t crtc_cookie =
            xcb_randr_get_crtc_info(conn_.get(), out_reply->crtc, res_reply->config_timestamp);
        xcb_randr_get_crtc_info_reply_t* crtc_reply = xcb_randr_get_crtc_info_reply(conn_.get(), crtc_cookie, nullptr);

        if (crtc_reply && crtc_reply->width > 0 && crtc_reply->height > 0)
        {
            Monitor monitor;
            monitor.output = outputs[i];
            monitor.name = output_name;
            monitor.x = crtc_reply->x;
            monitor.y = crtc_reply->y;
            monitor.width = crtc_reply->width;
            monitor.height = crtc_reply->height;

            std::cout << "Monitor: " << monitor.name << " at (" << monitor.x << ", " << monitor.y << ") size "
                      << monitor.width << "x" << monitor.height << std::endl;

            monitors_.push_back(monitor);
        }

        free(crtc_reply);
        free(out_reply);
    }

    free(res_reply);

    if (monitors_.empty())
    {
        std::cout << "No active monitors found, using fallback" << std::endl;
        create_fallback_monitor();
        return;
    }

    // Sort monitors by x-coordinate (left to right)
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
    for (auto& monitor : monitors_)
    {
        if (monitor.bar_window != XCB_NONE)
        {
            bar_.destroy(monitor.bar_window);
        }
        monitor.bar_window = bar_.create_for_monitor(monitor);
    }
}

void WindowManager::handle_event(xcb_generic_event_t const& event)
{
    uint8_t response_type = event.response_type & ~0x80;

    // Check for RANDR events
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
            handle_unmap_notify(reinterpret_cast<xcb_unmap_notify_event_t const&>(event));
            break;
        case XCB_DESTROY_NOTIFY:
            handle_destroy_notify(reinterpret_cast<xcb_destroy_notify_event_t const&>(event));
            break;
        case XCB_ENTER_NOTIFY:
            handle_enter_notify(reinterpret_cast<xcb_enter_notify_event_t const&>(event));
            break;
        case XCB_KEY_PRESS:
            handle_key_press(reinterpret_cast<xcb_key_press_event_t const&>(event));
            break;
    }
}

void WindowManager::handle_map_request(xcb_map_request_event_t const& e)
{
    manage_window(e.window);
    xcb_map_window(conn_.get(), e.window);
    focus_window(e.window);
}

void WindowManager::handle_unmap_notify(xcb_unmap_notify_event_t const& e) { unmanage_window(e.window); }

void WindowManager::handle_destroy_notify(xcb_destroy_notify_event_t const& e) { unmanage_window(e.window); }

void WindowManager::handle_enter_notify(xcb_enter_notify_event_t const& e)
{
    // Ignore enter events for bar windows
    for (auto const& monitor : monitors_)
    {
        if (e.event == monitor.bar_window)
            return;
    }

    if (e.event != conn_.screen()->root && e.mode == XCB_NOTIFY_MODE_NORMAL)
    {
        focus_window(e.event);
    }
}

void WindowManager::handle_key_press(xcb_key_press_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_press_event_t*>(&e), 0);
    std::cout << "Received key press: modifier=" << e.state << ", keysym=" << keysym << std::endl;

    auto action = keybinds_.resolve(e.state, keysym);
    if (action)
    {
        std::cout << "Executing action: " << action->type << std::endl;

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
            std::string path = keybinds_.resolve_command(action->command, config_);
            launch_program(path);
        }
    }
    else
    {
        std::cout << "No action associated with this key binding" << std::endl;
    }
}

void WindowManager::handle_randr_screen_change()
{
    std::cout << "Screen configuration changed, re-detecting monitors..." << std::endl;

    // Collect all windows from all monitors
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

    // Re-detect monitors
    detect_monitors();
    setup_monitor_bars();

    // Move all windows to the primary (first) monitor's current workspace
    if (!monitors_.empty())
    {
        auto& primary = monitors_[0];
        for (auto& window : all_windows)
        {
            primary.current().windows.push_back(window);
        }
    }

    focused_monitor_ = 0;
    rearrange_all_monitors();
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

    keybinds_.grab_keys(window);
    rearrange_monitor(focused_monitor());
    update_all_bars();
}

void WindowManager::unmanage_window(xcb_window_t window)
{
    // Search all monitors for the window
    for (auto& monitor : monitors_)
    {
        for (auto& workspace : monitor.workspaces)
        {
            auto it = std::ranges::find_if(workspace.windows, [window](Window const& w) { return w.id == window; });
            if (it != workspace.windows.end())
            {
                workspace.windows.erase(it);
                if (workspace.focused_window == window)
                {
                    workspace.focused_window = XCB_NONE;
                    if (!workspace.windows.empty())
                    {
                        workspace.focused_window = workspace.windows.back().id;
                    }
                }
                rearrange_monitor(monitor);
                update_all_bars();
                return;
            }
        }
    }
}

void WindowManager::focus_window(xcb_window_t window)
{
    // Find which monitor contains the window
    Monitor* target_monitor = monitor_containing_window(window);
    if (!target_monitor)
        return;

    // Update focused monitor
    for (size_t i = 0; i < monitors_.size(); ++i)
    {
        if (&monitors_[i] == target_monitor)
        {
            focused_monitor_ = i;
            break;
        }
    }

    auto& workspace = target_monitor->current();

    // Unfocus previous window
    if (workspace.focused_window != XCB_NONE && workspace.focused_window != window)
    {
        xcb_change_window_attributes(
            conn_.get(),
            workspace.focused_window,
            XCB_CW_BORDER_PIXEL,
            &conn_.screen()->black_pixel
        );
    }

    workspace.focused_window = window;
    xcb_change_window_attributes(
        conn_.get(),
        workspace.focused_window,
        XCB_CW_BORDER_PIXEL,
        &config_.appearance.border_color
    );
    xcb_configure_window(
        conn_.get(),
        workspace.focused_window,
        XCB_CONFIG_WINDOW_BORDER_WIDTH,
        &config_.appearance.border_width
    );
    xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, workspace.focused_window, XCB_CURRENT_TIME);

    update_all_bars();
    conn_.flush();
}

void WindowManager::kill_window(xcb_window_t window)
{
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn_.get(), 0, 16, "WM_DELETE_WINDOW");
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn_.get(), cookie, nullptr);

    if (reply)
    {
        xcb_client_message_event_t ev = { 0 };
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = window;
        ev.type = reply->atom;
        ev.format = 32;
        ev.data.data32[0] = reply->atom;
        ev.data.data32[1] = XCB_CURRENT_TIME;

        xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
        free(reply);
    }

    usleep(100000); // 100ms

    xcb_get_window_attributes_cookie_t attr_cookie = xcb_get_window_attributes(conn_.get(), window);
    xcb_get_window_attributes_reply_t* attr_reply = xcb_get_window_attributes_reply(conn_.get(), attr_cookie, nullptr);

    if (attr_reply)
    {
        free(attr_reply);
        xcb_kill_client(conn_.get(), window);
    }

    unmanage_window(window);
    conn_.flush();
}

void WindowManager::rearrange_monitor(Monitor& monitor)
{
    std::cout << "Rearranging monitor: " << monitor.name << " workspace: " << monitor.current_workspace << std::endl;

    // Unmap windows on non-current workspaces of this monitor
    for (size_t i = 0; i < monitor.workspaces.size(); ++i)
    {
        if (i != monitor.current_workspace)
        {
            for (auto const& window : monitor.workspaces[i].windows)
            {
                xcb_unmap_window(conn_.get(), window.id);
            }
        }
    }

    // Arrange current workspace windows
    layout_.arrange(monitor.current().windows, monitor.geometry());
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

    // Unmap current workspace windows
    for (auto const& window : monitor.current().windows)
    {
        xcb_unmap_window(conn_.get(), window.id);
    }

    // Switch workspace
    monitor.current_workspace = static_cast<size_t>(ws);

    // Arrange new workspace
    rearrange_monitor(monitor);

    // Focus appropriate window
    if (monitor.current().focused_window != XCB_NONE)
    {
        focus_window(monitor.current().focused_window);
    }
    else if (!monitor.current().windows.empty())
    {
        focus_window(monitor.current().windows.back().id);
    }

    update_all_bars();
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

    // Find and remove from current workspace
    auto it =
        std::ranges::find_if(current_ws.windows, [window_to_move](Window const& w) { return w.id == window_to_move; });
    if (it == current_ws.windows.end())
        return;

    Window moved_window = *it;
    current_ws.windows.erase(it);
    current_ws.focused_window = XCB_NONE;

    // Add to target workspace
    monitor.workspaces[ws].windows.push_back(moved_window);
    monitor.workspaces[ws].focused_window = window_to_move;

    // Unmap the moved window
    xcb_unmap_window(conn_.get(), window_to_move);

    // Rearrange current workspace
    rearrange_monitor(monitor);

    // Focus next window if available
    if (!current_ws.windows.empty())
    {
        focus_window(current_ws.windows.back().id);
    }

    update_all_bars();
}

size_t WindowManager::wrap_monitor_index(int idx) const
{
    int size = static_cast<int>(monitors_.size());
    return static_cast<size_t>(((idx % size) + size) % size);
}

void WindowManager::focus_monitor(int direction)
{
    if (monitors_.size() <= 1)
        return;

    focused_monitor_ = wrap_monitor_index(static_cast<int>(focused_monitor_) + direction);

    auto& monitor = focused_monitor();
    if (monitor.current().focused_window != XCB_NONE)
    {
        focus_window(monitor.current().focused_window);
    }
    else if (!monitor.current().windows.empty())
    {
        focus_window(monitor.current().windows.back().id);
    }

    // Warp pointer to center of new monitor
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

    update_all_bars();
    conn_.flush();
}

void WindowManager::move_window_to_monitor(int direction)
{
    if (monitors_.size() <= 1)
        return;

    auto& source_monitor = focused_monitor();
    auto& source_ws = source_monitor.current();

    if (source_ws.focused_window == XCB_NONE)
        return;

    size_t target_idx = wrap_monitor_index(static_cast<int>(focused_monitor_) + direction);
    if (target_idx == focused_monitor_)
        return;

    auto& target_monitor = monitors_[target_idx];
    auto& target_ws = target_monitor.current();

    xcb_window_t window_to_move = source_ws.focused_window;

    // Find and remove from source workspace
    auto it =
        std::ranges::find_if(source_ws.windows, [window_to_move](Window const& w) { return w.id == window_to_move; });
    if (it == source_ws.windows.end())
        return;

    Window moved_window = *it;
    source_ws.windows.erase(it);
    source_ws.focused_window = XCB_NONE;

    // Add to target workspace
    target_ws.windows.push_back(moved_window);
    target_ws.focused_window = window_to_move;

    // Rearrange both monitors
    rearrange_monitor(source_monitor);
    rearrange_monitor(target_monitor);

    // Focus window on target monitor
    focused_monitor_ = target_idx;
    focus_window(window_to_move);

    // Warp pointer to target monitor
    xcb_warp_pointer(
        conn_.get(),
        XCB_NONE,
        conn_.screen()->root,
        0,
        0,
        0,
        0,
        target_monitor.x + target_monitor.width / 2,
        target_monitor.y + target_monitor.height / 2
    );

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
    for (auto& monitor : monitors_)
    {
        auto& ws = monitor.current();
        auto it = std::ranges::find_if(ws.windows, [window](Window const& w) { return w.id == window; });
        if (it != ws.windows.end())
        {
            return &monitor;
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
    xcb_get_property_cookie_t cookie =
        xcb_get_property(conn_.get(), 0, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 1024);
    xcb_get_property_reply_t* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);

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
    for (auto const& monitor : monitors_)
    {
        bar_.update(monitor);
    }
}

} // namespace lwm
