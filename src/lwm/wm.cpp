#include "wm.hpp"
#include "lwm/core/floating.hpp"
#include "lwm/core/focus.hpp"
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

constexpr uint32_t WM_STATE_NORMAL = 1;
constexpr uint32_t WM_STATE_ICONIC = 3;

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
          config_.appearance.enable_internal_bar
              ? std::optional<StatusBar>(std::in_place, conn_, config_.appearance, config_.workspaces.names)
                                                 : std::nullopt
      )
{
    setup_signal_handlers();
    create_wm_window();
    setup_root();
    grab_buttons();
    claim_wm_ownership();
    wm_transient_for_ = intern_atom("WM_TRANSIENT_FOR");
    wm_state_ = intern_atom("WM_STATE");
    wm_change_state_ = intern_atom("WM_CHANGE_STATE");
    utf8_string_ = intern_atom("UTF8_STRING");
    detect_monitors();
    setup_ewmh();
    if (bar_)
    {
        setup_monitor_bars();
    }
    scan_existing_windows();
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
                          | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_POINTER_MOTION
                          | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE
                          | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE };
    auto cookie =
        xcb_change_window_attributes_checked(conn_.get(), conn_.screen()->root, XCB_CW_EVENT_MASK, values);
    if (auto* err = xcb_request_check(conn_.get(), cookie))
    {
        free(err);
        throw std::runtime_error("Another window manager is already running");
    }

    if (conn_.has_randr())
    {
        xcb_randr_select_input(conn_.get(), conn_.screen()->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }
}

void WindowManager::create_wm_window()
{
    wm_window_ = xcb_generate_id(conn_.get());
    xcb_create_window(
        conn_.get(),
        XCB_COPY_FROM_PARENT,
        wm_window_,
        conn_.screen()->root,
        -1,
        -1,
        1,
        1,
        0,
        XCB_WINDOW_CLASS_INPUT_ONLY,
        XCB_COPY_FROM_PARENT,
        0,
        nullptr
    );
}

void WindowManager::grab_buttons()
{
    xcb_window_t root = conn_.screen()->root;
    xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, root, XCB_MOD_MASK_ANY);

    uint16_t buttons[] = { XCB_BUTTON_INDEX_1, XCB_BUTTON_INDEX_3 };
    uint16_t modifiers[] = { XCB_MOD_MASK_4,
                             static_cast<uint16_t>(XCB_MOD_MASK_4 | XCB_MOD_MASK_2),
                             static_cast<uint16_t>(XCB_MOD_MASK_4 | XCB_MOD_MASK_LOCK),
                             static_cast<uint16_t>(XCB_MOD_MASK_4 | XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK) };

    for (auto button : buttons)
    {
        for (auto mod : modifiers)
        {
            xcb_grab_button(
                conn_.get(),
                0,
                root,
                XCB_EVENT_MASK_BUTTON_PRESS,
                XCB_GRAB_MODE_ASYNC,
                XCB_GRAB_MODE_ASYNC,
                XCB_NONE,
                XCB_NONE,
                button,
                mod
            );
        }
    }

    conn_.flush();
}

void WindowManager::claim_wm_ownership()
{
    xcb_atom_t wm_s0 = intern_atom("WM_S0");
    if (wm_s0 == XCB_NONE)
        throw std::runtime_error("Failed to intern WM_S0 atom");

    auto owner_cookie = xcb_get_selection_owner(conn_.get(), wm_s0);
    auto* owner_reply = xcb_get_selection_owner_reply(conn_.get(), owner_cookie, nullptr);
    if (!owner_reply)
        throw std::runtime_error("Failed to query WM selection owner");

    if (owner_reply->owner != XCB_NONE)
    {
        free(owner_reply);
        throw std::runtime_error("Another window manager already owns WM_S0");
    }
    free(owner_reply);

    xcb_set_selection_owner(conn_.get(), wm_window_, wm_s0, XCB_CURRENT_TIME);

    owner_cookie = xcb_get_selection_owner(conn_.get(), wm_s0);
    owner_reply = xcb_get_selection_owner_reply(conn_.get(), owner_cookie, nullptr);
    if (!owner_reply || owner_reply->owner != wm_window_)
    {
        if (owner_reply)
            free(owner_reply);
        throw std::runtime_error("Failed to acquire WM_S0 selection");
    }
    free(owner_reply);
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
            init_monitor_workspaces(monitor);
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
    init_monitor_workspaces(monitor);
    monitors_.push_back(monitor);
}

void WindowManager::init_monitor_workspaces(Monitor& monitor)
{
    monitor.workspaces.assign(config_.workspaces.count, Workspace{});
    monitor.current_workspace = 0;
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

void WindowManager::scan_existing_windows()
{
    auto cookie = xcb_query_tree(conn_.get(), conn_.screen()->root);
    auto* reply = xcb_query_tree_reply(conn_.get(), cookie, nullptr);
    if (!reply)
        return;

    int length = xcb_query_tree_children_length(reply);
    xcb_window_t* children = xcb_query_tree_children(reply);

    suppress_focus_ = true;
    for (int i = 0; i < length; ++i)
    {
        xcb_window_t window = children[i];
        auto attr_cookie = xcb_get_window_attributes(conn_.get(), window);
        auto* attr_reply = xcb_get_window_attributes_reply(conn_.get(), attr_cookie, nullptr);
        if (!attr_reply)
            continue;

        bool is_viewable = attr_reply->map_state == XCB_MAP_STATE_VIEWABLE;
        bool override_redirect = attr_reply->override_redirect;
        free(attr_reply);

        if (!is_viewable || override_redirect)
            continue;

        if (ewmh_.is_dock_window(window))
        {
            uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_POINTER_MOTION };
            xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
            if (std::ranges::find(dock_windows_, window) == dock_windows_.end())
            {
                dock_windows_.push_back(window);
                update_struts();
            }
            continue;
        }

        xcb_atom_t type = ewmh_.get_window_type(window);
        bool is_menu = type == ewmh_.get()->_NET_WM_WINDOW_TYPE_MENU
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DROPDOWN_MENU
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_POPUP_MENU
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_TOOLTIP
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_NOTIFICATION
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_COMBO
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DND;
        bool is_floating_type = type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DIALOG
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_UTILITY
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_SPLASH;
        bool has_transient = transient_for_window(window).has_value();

        if (is_menu)
            continue;

        if (is_floating_type || has_transient)
        {
            manage_floating_window(window);
            continue;
        }

        if (!ewmh_.should_tile_window(window))
            continue;

        manage_window(window);
    }

    suppress_focus_ = false;
    free(reply);

    rearrange_all_monitors();

    auto pointer_cookie = xcb_query_pointer(conn_.get(), conn_.screen()->root);
    auto* pointer_reply = xcb_query_pointer_reply(conn_.get(), pointer_cookie, nullptr);
    if (pointer_reply)
    {
        auto monitor_idx =
            focus::monitor_index_at_point(monitors_, pointer_reply->root_x, pointer_reply->root_y).value_or(0);
        focused_monitor_ = monitor_idx;
        free(pointer_reply);
    }

    if (!monitors_.empty())
        focus_or_fallback(monitors_[focused_monitor_]);
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
        case XCB_MOTION_NOTIFY:
            handle_motion_notify(reinterpret_cast<xcb_motion_notify_event_t const&>(event));
            break;
        case XCB_BUTTON_PRESS:
            handle_button_press(reinterpret_cast<xcb_button_press_event_t const&>(event));
            break;
        case XCB_BUTTON_RELEASE:
            handle_button_release(reinterpret_cast<xcb_button_release_event_t const&>(event));
            break;
        case XCB_KEY_PRESS:
            handle_key_press(reinterpret_cast<xcb_key_press_event_t const&>(event));
            break;
        case XCB_CLIENT_MESSAGE:
            handle_client_message(reinterpret_cast<xcb_client_message_event_t const&>(event));
            break;
        case XCB_CONFIGURE_REQUEST:
            handle_configure_request(reinterpret_cast<xcb_configure_request_event_t const&>(event));
            break;
        case XCB_PROPERTY_NOTIFY:
            handle_property_notify(reinterpret_cast<xcb_property_notify_event_t const&>(event));
            break;
    }
}

void WindowManager::handle_map_request(xcb_map_request_event_t const& e)
{
    if (monitor_containing_window(e.window) || find_floating_window(e.window))
    {
        bool focus = false;
        auto monitor_idx = monitor_index_for_window(e.window);
        auto workspace_idx = workspace_index_for_window(e.window);
        if (monitor_idx && workspace_idx && *monitor_idx < monitors_.size())
        {
            focus = *monitor_idx == focused_monitor_
                && *workspace_idx == monitors_[*monitor_idx].current_workspace;
        }
        deiconify_window(e.window, focus);
        return;
    }

    // Check if this is a dock window (e.g., Polybar)
    if (ewmh_.is_dock_window(e.window))
    {
        // Map but don't manage - let it float above
        uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_POINTER_MOTION };
        xcb_change_window_attributes(conn_.get(), e.window, XCB_CW_EVENT_MASK, values);
        xcb_map_window(conn_.get(), e.window);
        dock_windows_.push_back(e.window);
        update_struts();
        rearrange_all_monitors();
        conn_.flush();
        return;
    }

    if (is_override_redirect_window(e.window))
    {
        xcb_map_window(conn_.get(), e.window);
        conn_.flush();
        return;
    }

    xcb_atom_t type = ewmh_.get_window_type(e.window);
    bool is_menu = type == ewmh_.get()->_NET_WM_WINDOW_TYPE_MENU
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DROPDOWN_MENU
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_POPUP_MENU
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_TOOLTIP
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_NOTIFICATION
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_COMBO
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DND;
    bool is_floating_type = type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DIALOG
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_UTILITY
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_SPLASH;
    bool has_transient = transient_for_window(e.window).has_value();

    // Ignore short-lived popup windows and tooltips.
    if (is_menu)
    {
        xcb_map_window(conn_.get(), e.window);
        conn_.flush();
        return;
    }

    if (is_floating_type || has_transient)
    {
        manage_floating_window(e.window);
        return;
    }

    // Check if window should not be tiled (dialogs, menus, etc.)
    if (!ewmh_.should_tile_window(e.window))
    {
        // Map but don't tile - let it float unmanaged
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
    unmanage_floating_window(window);
    unmanage_window(window);
}

void WindowManager::handle_enter_notify(xcb_enter_notify_event_t const& e)
{
    if (drag_state_.active)
        return;

    // For non-root windows: only handle if mode=NORMAL and detail!=INFERIOR
    // detail=INFERIOR means pointer entered because a child window closed,
    // not because the mouse actually moved - ignore these spurious events.
    // For root window: always handle (following DWM behavior)
    if (e.event != conn_.screen()->root)
    {
        if (e.mode != XCB_NOTIFY_MODE_NORMAL || e.detail == XCB_NOTIFY_DETAIL_INFERIOR)
            return;
    }
    else
    {
        // Root window: only require normal mode
        if (e.mode != XCB_NOTIFY_MODE_NORMAL)
            return;
    }

    // Case 1: Entering a managed window - focus-follows-mouse
    if (e.event != conn_.screen()->root)
    {
        if (find_floating_window(e.event))
        {
            focus_floating_window(e.event);
            return;
        }
        if (monitor_containing_window(e.event))
        {
            focus_window(e.event);
            return;
        }
    }

    // Case 2: Entering root or unmanaged window area (gaps/empty space)
    update_focused_monitor_at_point(e.root_x, e.root_y);
}

void WindowManager::handle_motion_notify(xcb_motion_notify_event_t const& e)
{
    if (drag_state_.active)
    {
        update_drag(e.root_x, e.root_y);
        return;
    }

    if (e.event != conn_.screen()->root)
    {
        if (find_floating_window(e.event) || monitor_containing_window(e.event))
            return;
    }

    update_focused_monitor_at_point(e.root_x, e.root_y);
}

void WindowManager::handle_button_press(xcb_button_press_event_t const& e)
{
    uint16_t clean_mod = e.state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2);
    xcb_window_t target = e.event;
    if (target == conn_.screen()->root && e.child != XCB_NONE)
        target = e.child;

    if (clean_mod & XCB_MOD_MASK_4)
    {
        if (find_floating_window(target))
        {
            bool is_resize = e.detail == XCB_BUTTON_INDEX_3;
            bool is_move = e.detail == XCB_BUTTON_INDEX_1;
            if (is_move || is_resize)
            {
                focus_floating_window(target);
                begin_drag(target, is_resize, e.root_x, e.root_y);
                return;
            }
        }
        if (monitor_containing_window(target))
            return;
    }

    // Only handle clicks on root window (empty areas or gaps)
    if (e.event != conn_.screen()->root)
        return;

    // Update focused monitor based on click position
    update_focused_monitor_at_point(e.root_x, e.root_y);
}

void WindowManager::handle_button_release(xcb_button_release_event_t const& /*e*/)
{
    if (!drag_state_.active)
        return;

    end_drag();
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

    if (action->type == "kill" && active_window_ != XCB_NONE)
    {
        kill_window(active_window_);
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

    if (e.type == wm_change_state_)
    {
        if (e.data.data32[0] == WM_STATE_ICONIC)
        {
            iconify_window(e.window);
        }
        return;
    }

    if (e.type == ewmh->_NET_WM_STATE)
    {
        uint32_t action = e.data.data32[0];
        xcb_atom_t first = static_cast<xcb_atom_t>(e.data.data32[1]);
        xcb_atom_t second = static_cast<xcb_atom_t>(e.data.data32[2]);

        auto handle_state = [&](xcb_atom_t state) {
            if (state == XCB_ATOM_NONE)
                return;

            if (state == ewmh->_NET_WM_STATE_FULLSCREEN)
            {
                bool enable = action == 1
                    || (action == 2 && !fullscreen_windows_.contains(e.window));
                if (action == 0 || (action == 2 && fullscreen_windows_.contains(e.window)))
                    enable = false;
                set_fullscreen(e.window, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_ABOVE)
            {
                bool enable = action == 1
                    || (action == 2 && !above_windows_.contains(e.window));
                if (action == 0 || (action == 2 && above_windows_.contains(e.window)))
                    enable = false;
                set_window_above(e.window, enable);
            }
        };

        handle_state(first);
        handle_state(second);
        return;
    }

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
        if (iconic_windows_.contains(window))
        {
            deiconify_window(window, false);
        }
        if (monitor_containing_window(window))
        {
            focus_window(window);
        }
        else if (find_floating_window(window))
        {
            focus_floating_window(window);
        }
    }
}

void WindowManager::handle_configure_request(xcb_configure_request_event_t const& e)
{
    if (monitor_containing_window(e.window))
    {
        send_configure_notify(e.window);
        return;
    }

    if (fullscreen_windows_.contains(e.window))
    {
        apply_fullscreen_if_needed(e.window);
        send_configure_notify(e.window);
        return;
    }

    auto* floating_window = find_floating_window(e.window);
    uint16_t mask = e.value_mask;
    if (floating_window)
    {
        mask &= ~XCB_CONFIG_WINDOW_BORDER_WIDTH;
    }

    if (mask == 0)
        return;

    uint32_t values[7];
    size_t index = 0;

    if (mask & XCB_CONFIG_WINDOW_X)
        values[index++] = static_cast<uint32_t>(e.x);
    if (mask & XCB_CONFIG_WINDOW_Y)
        values[index++] = static_cast<uint32_t>(e.y);
    if (mask & XCB_CONFIG_WINDOW_WIDTH)
        values[index++] = static_cast<uint32_t>(e.width);
    if (mask & XCB_CONFIG_WINDOW_HEIGHT)
        values[index++] = static_cast<uint32_t>(e.height);
    if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
        values[index++] = static_cast<uint32_t>(e.border_width);
    if (mask & XCB_CONFIG_WINDOW_SIBLING)
        values[index++] = static_cast<uint32_t>(e.sibling);
    if (mask & XCB_CONFIG_WINDOW_STACK_MODE)
        values[index++] = static_cast<uint32_t>(e.stack_mode);

    xcb_configure_window(conn_.get(), e.window, mask, values);

    if (floating_window)
    {
        if (mask & XCB_CONFIG_WINDOW_X)
            floating_window->geometry.x = e.x;
        if (mask & XCB_CONFIG_WINDOW_Y)
            floating_window->geometry.y = e.y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH)
            floating_window->geometry.width = std::max<uint16_t>(1, e.width);
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)
            floating_window->geometry.height = std::max<uint16_t>(1, e.height);

        update_floating_monitor_for_geometry(*floating_window);
        if (active_window_ == floating_window->id)
        {
            focused_monitor_ = floating_window->monitor;
            update_ewmh_current_desktop();
            update_all_bars();
        }
    }

    conn_.flush();
}

void WindowManager::handle_property_notify(xcb_property_notify_event_t const& e)
{
    if (e.atom == ewmh_.get()->_NET_WM_NAME || e.atom == XCB_ATOM_WM_NAME)
    {
        update_window_title(e.window);
    }
}

void WindowManager::handle_randr_screen_change()
{
    std::vector<Window> all_windows;
    std::vector<FloatingWindow> all_floating = floating_windows_;
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

        floating_windows_.clear();
        for (auto& floating_window : all_floating)
        {
            floating_window.monitor = 0;
            floating_window.workspace = monitors_[0].current_workspace;
            floating_window.geometry = floating::place_floating(
                monitors_[0].working_area(),
                floating_window.geometry.width,
                floating_window.geometry.height,
                std::nullopt
            );
            floating_windows_.push_back(floating_window);
            ewmh_.set_window_desktop(floating_window.id, desktop);
        }
    }

    // Update EWMH for new monitor configuration
    update_ewmh_desktops();
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

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_NORMAL, 0 };
        xcb_change_property(
            conn_.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            wm_state_,
            wm_state_,
            32,
            2,
            data
        );
    }

    // Set EWMH desktop for this window
    uint32_t desktop = get_ewmh_desktop_index(focused_monitor_, focused_monitor().current_workspace);
    ewmh_.set_window_desktop(window, desktop);
    update_ewmh_client_list();

    keybinds_.grab_keys(window);
    rearrange_monitor(focused_monitor());
    update_all_bars();

    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN))
    {
        set_fullscreen(window, true);
    }
    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE))
    {
        set_window_above(window, true);
    }
}

void WindowManager::manage_floating_window(xcb_window_t window)
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

    if (!monitor_idx)
        monitor_idx = focused_monitor_;
    if (!workspace_idx)
        workspace_idx = monitors_[*monitor_idx].current_workspace;

    uint16_t width = 300;
    uint16_t height = 200;
    auto geom_cookie = xcb_get_geometry(conn_.get(), window);
    auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
    if (geom_reply)
    {
        width = geom_reply->width;
        height = geom_reply->height;
        free(geom_reply);
    }
    if (width == 0)
        width = 300;
    if (height == 0)
        height = 200;

    Geometry placement = floating::place_floating(
        monitors_[*monitor_idx].working_area(),
        width,
        height,
        parent_geom
    );

    FloatingWindow floating_window;
    floating_window.id = window;
    floating_window.monitor = *monitor_idx;
    floating_window.workspace = *workspace_idx;
    floating_window.geometry = placement;
    floating_windows_.push_back(floating_window);

    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
                          | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_NORMAL, 0 };
        xcb_change_property(
            conn_.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            wm_state_,
            wm_state_,
            32,
            2,
            data
        );
    }

    uint32_t desktop = get_ewmh_desktop_index(*monitor_idx, *workspace_idx);
    ewmh_.set_window_desktop(window, desktop);
    update_ewmh_client_list();

    keybinds_.grab_keys(window);
    update_floating_visibility(*monitor_idx);
    if (!suppress_focus_)
        focus_floating_window(window);

    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN))
    {
        set_fullscreen(window, true);
    }
    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE))
    {
        set_window_above(window, true);
    }
}

void WindowManager::unmanage_window(xcb_window_t window)
{
    fullscreen_windows_.erase(window);
    fullscreen_restore_.erase(window);
    above_windows_.erase(window);
    iconic_windows_.erase(window);

    for (auto& monitor : monitors_)
    {
        for (size_t ws_idx = 0; ws_idx < monitor.workspaces.size(); ++ws_idx)
        {
            auto& workspace = monitor.workspaces[ws_idx];
            auto it = workspace.find_window(window);
            if (it != workspace.windows.end())
            {
                workspace.windows.erase(it);
                bool was_active = (active_window_ == window);
                bool was_workspace_focus = (workspace.focused_window == window);
                if (was_workspace_focus)
                {
                    workspace.focused_window = workspace.windows.empty() ? XCB_NONE : workspace.windows.back().id;
                }
                update_ewmh_client_list();
                rearrange_monitor(monitor);

                // If this was the active window, select a new focus or clear focus
                if (was_active)
                {
                    if (ws_idx == monitor.current_workspace && &monitor == &focused_monitor())
                    {
                        focus_or_fallback(monitor);
                    }
                    else
                    {
                        clear_focus();
                    }
                }

                update_all_bars();
                conn_.flush();
                return;
            }
        }
    }
}

void WindowManager::unmanage_floating_window(xcb_window_t window)
{
    fullscreen_windows_.erase(window);
    fullscreen_restore_.erase(window);
    above_windows_.erase(window);
    iconic_windows_.erase(window);

    auto it = std::ranges::find_if(
        floating_windows_,
        [window](FloatingWindow const& floating_window) { return floating_window.id == window; }
    );
    if (it == floating_windows_.end())
        return;

    bool was_active = (active_window_ == window);
    size_t monitor_idx = it->monitor;
    size_t workspace_idx = it->workspace;
    floating_windows_.erase(it);
    update_ewmh_client_list();

    if (was_active)
    {
        if (monitor_idx == focused_monitor_ && workspace_idx == monitors_[monitor_idx].current_workspace)
        {
            auto& ws = monitors_[monitor_idx].current();
            if (ws.focused_window != XCB_NONE)
            {
                focus_window(ws.focused_window);
            }
            else
            {
                auto it2 = std::find_if(
                    floating_windows_.rbegin(),
                    floating_windows_.rend(),
                    [&](FloatingWindow const& floating_window) {
                        return floating_window.monitor == monitor_idx && floating_window.workspace == workspace_idx;
                    }
                );
                if (it2 != floating_windows_.rend())
                {
                    focus_floating_window(it2->id);
                }
                else
                {
                    clear_focus();
                }
            }
        }
        else
        {
            clear_focus();
        }
    }

    update_all_bars();
    conn_.flush();
}

void WindowManager::focus_window(xcb_window_t window)
{
    if (iconic_windows_.contains(window))
    {
        deiconify_window(window, false);
    }

    auto change = focus::focus_window_state(monitors_, focused_monitor_, active_window_, window);
    if (!change)
        return;

    auto& target_monitor = monitors_[change->target_monitor];
    if (change->workspace_changed)
    {
        for (auto const& w : target_monitor.workspaces[change->old_workspace].windows)
        {
            wm_unmap_window(w.id);
        }
        rearrange_monitor(target_monitor);
        update_floating_visibility(change->target_monitor);
    }

    update_ewmh_current_desktop();

    // Clear borders on all windows across all monitors
    clear_all_borders();

    xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &config_.appearance.border_color);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);
    xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME);

    // Clear urgent hint when window receives focus
    ewmh_.set_demands_attention(window, false);
    ewmh_.set_active_window(window);

    apply_fullscreen_if_needed(window);

    update_all_bars();
    conn_.flush();
}

void WindowManager::focus_floating_window(xcb_window_t window)
{
    auto* floating_window = find_floating_window(window);
    if (!floating_window)
        return;

    if (iconic_windows_.contains(window))
    {
        deiconify_window(window, false);
    }

    if (floating_window->monitor >= monitors_.size())
        return;

    focused_monitor_ = floating_window->monitor;
    auto& monitor = monitors_[floating_window->monitor];
    if (monitor.current_workspace != floating_window->workspace)
    {
        for (auto const& w : monitor.current().windows)
        {
            wm_unmap_window(w.id);
        }
        monitor.current_workspace = floating_window->workspace;
        rearrange_monitor(monitor);
        update_floating_visibility(floating_window->monitor);
    }

    update_ewmh_current_desktop();

    // Keep most-recently-focused floating window at the end.
    auto it = std::find_if(
        floating_windows_.begin(),
        floating_windows_.end(),
        [window](FloatingWindow const& fw) { return fw.id == window; }
    );
    if (it != floating_windows_.end() && (it + 1) != floating_windows_.end())
    {
        FloatingWindow saved = *it;
        floating_windows_.erase(it);
        floating_windows_.push_back(saved);
        floating_window = &floating_windows_.back();
    }

    active_window_ = window;
    clear_all_borders();
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &config_.appearance.border_color);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);
    xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME);

    uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);

    ewmh_.set_demands_attention(window, false);
    ewmh_.set_active_window(window);

    apply_fullscreen_if_needed(window);

    update_all_bars();
    conn_.flush();
}

void WindowManager::set_fullscreen(xcb_window_t window, bool enabled)
{
    auto monitor_idx = monitor_index_for_window(window);
    if (!monitor_idx)
        return;

    if (enabled)
    {
        if (!fullscreen_windows_.contains(window))
        {
            if (auto* floating_window = find_floating_window(window))
            {
                fullscreen_restore_[window] = floating_window->geometry;
            }
            else
            {
                auto geom_cookie = xcb_get_geometry(conn_.get(), window);
                auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
                if (geom_reply)
                {
                    fullscreen_restore_[window] =
                        Geometry{ geom_reply->x, geom_reply->y, geom_reply->width, geom_reply->height };
                    free(geom_reply);
                }
            }
        }

        fullscreen_windows_.insert(window);
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN, true);
        apply_fullscreen_if_needed(window);
    }
    else
    {
        if (!fullscreen_windows_.contains(window))
            return;

        fullscreen_windows_.erase(window);
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN, false);

        if (auto* floating_window = find_floating_window(window))
        {
            auto restore = fullscreen_restore_.find(window);
            if (restore != fullscreen_restore_.end())
            {
                floating_window->geometry = restore->second;
                if (floating_window->workspace == monitors_[floating_window->monitor].current_workspace
                    && !iconic_windows_.contains(window))
                {
                    apply_floating_geometry(*floating_window);
                }
            }
        }
        else if (*monitor_idx < monitors_.size())
        {
            auto workspace_idx = workspace_index_for_window(window);
            if (workspace_idx && *workspace_idx == monitors_[*monitor_idx].current_workspace)
            {
                rearrange_monitor(monitors_[*monitor_idx]);
            }
        }

        fullscreen_restore_.erase(window);
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);
    }

    update_all_bars();
    conn_.flush();
}

void WindowManager::set_window_above(xcb_window_t window, bool enabled)
{
    if (!monitor_index_for_window(window))
        return;

    if (enabled)
    {
        above_windows_.insert(window);
        uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
    }
    else
    {
        above_windows_.erase(window);
    }

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE, enabled);
    conn_.flush();
}

void WindowManager::apply_fullscreen_if_needed(xcb_window_t window)
{
    if (!fullscreen_windows_.contains(window))
        return;
    if (iconic_windows_.contains(window))
        return;

    std::optional<size_t> monitor_idx;
    if (auto* floating_window = find_floating_window(window))
    {
        monitor_idx = floating_window->monitor;
        if (monitor_idx && *monitor_idx < monitors_.size())
        {
            if (floating_window->workspace != monitors_[*monitor_idx].current_workspace)
                return;
        }
    }
    else
    {
        monitor_idx = monitor_index_for_window(window);
        auto workspace_idx = workspace_index_for_window(window);
        if (!monitor_idx || !workspace_idx || *monitor_idx >= monitors_.size()
            || *workspace_idx != monitors_[*monitor_idx].current_workspace)
            return;
    }

    Geometry area = monitors_[*monitor_idx].geometry();
    uint32_t values[] = {
        static_cast<uint32_t>(area.x),
        static_cast<uint32_t>(area.y),
        area.width,
        area.height,
        0
    };
    uint16_t mask =
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
        | XCB_CONFIG_WINDOW_BORDER_WIDTH;
    xcb_configure_window(conn_.get(), window, mask, values);

    uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
}

void WindowManager::iconify_window(xcb_window_t window)
{
    auto monitor_idx = monitor_index_for_window(window);
    if (!monitor_idx)
        return;

    if (iconic_windows_.contains(window))
        return;

    iconic_windows_.insert(window);

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_ICONIC, 0 };
        xcb_change_property(
            conn_.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            wm_state_,
            wm_state_,
            32,
            2,
            data
        );
    }

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_HIDDEN, true);

    if (auto* floating_window = find_floating_window(window))
    {
        wm_unmap_window(window);
        update_floating_visibility(floating_window->monitor);
    }
    else if (*monitor_idx < monitors_.size())
    {
        wm_unmap_window(window);
        rearrange_monitor(monitors_[*monitor_idx]);
    }

    if (active_window_ == window)
    {
        auto workspace_idx = workspace_index_for_window(window);
        if (workspace_idx && *monitor_idx == focused_monitor_
            && *workspace_idx == monitors_[*monitor_idx].current_workspace)
        {
            focus_or_fallback(monitors_[*monitor_idx]);
        }
        else
        {
            clear_focus();
        }
    }

    update_all_bars();
    conn_.flush();
}

void WindowManager::deiconify_window(xcb_window_t window, bool focus)
{
    auto monitor_idx = monitor_index_for_window(window);
    if (!monitor_idx)
        return;

    iconic_windows_.erase(window);

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_NORMAL, 0 };
        xcb_change_property(
            conn_.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            wm_state_,
            wm_state_,
            32,
            2,
            data
        );
    }

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_HIDDEN, false);

    if (auto* floating_window = find_floating_window(window))
    {
        update_floating_visibility(floating_window->monitor);
        if (focus && floating_window->monitor == focused_monitor_
            && floating_window->workspace == monitors_[floating_window->monitor].current_workspace)
        {
            focus_floating_window(window);
        }
    }
    else if (*monitor_idx < monitors_.size())
    {
        auto workspace_idx = workspace_index_for_window(window);
        rearrange_monitor(monitors_[*monitor_idx]);
        if (focus && workspace_idx && *monitor_idx == focused_monitor_
            && *workspace_idx == monitors_[*monitor_idx].current_workspace)
        {
            focus_window(window);
        }
    }

    apply_fullscreen_if_needed(window);
    update_all_bars();
    conn_.flush();
}

void WindowManager::clear_focus()
{
    clear_all_borders();
    active_window_ = XCB_NONE;
    xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, XCB_CURRENT_TIME);
    ewmh_.set_active_window(XCB_NONE);
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
    std::vector<Window> visible_windows;
    visible_windows.reserve(monitor.current().windows.size());
    for (auto const& window : monitor.current().windows)
    {
        if (iconic_windows_.contains(window.id))
        {
            wm_unmap_window(window.id);
            continue;
        }
        visible_windows.push_back(window);
    }

    layout_.arrange(visible_windows, monitor.working_area(), bar_.has_value());

    for (auto const& window : visible_windows)
    {
        apply_fullscreen_if_needed(window.id);
    }

    // Clear unmap tracking for windows that are now visible
    for (auto const& window : visible_windows)
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
    update_floating_visibility_all();
}

void WindowManager::switch_workspace(int ws)
{
    auto& monitor = focused_monitor();
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count == 0)
        return;

    if (ws < 0 || static_cast<size_t>(ws) >= workspace_count || static_cast<size_t>(ws) == monitor.current_workspace)
        return;

    for (auto const& window : monitor.current().windows)
    {
        wm_unmap_window(window.id);
    }

    monitor.current_workspace = static_cast<size_t>(ws);
    update_ewmh_current_desktop();
    rearrange_monitor(monitor);
    update_floating_visibility(focused_monitor_);
    focus_or_fallback(monitor);
    update_all_bars();
    conn_.flush();
}

void WindowManager::move_window_to_workspace(int ws)
{
    auto& monitor = focused_monitor();
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count == 0)
        return;

    if (ws < 0 || static_cast<size_t>(ws) >= workspace_count || static_cast<size_t>(ws) == monitor.current_workspace)
        return;

    if (active_window_ == XCB_NONE)
        return;

    auto& current_ws = monitor.current();

    xcb_window_t window_to_move = active_window_;

    auto it = current_ws.find_window(window_to_move);
    if (it == current_ws.windows.end())
        return;

    Window moved_window = *it;
    current_ws.windows.erase(it);
    current_ws.focused_window = XCB_NONE;

    size_t target_ws = static_cast<size_t>(ws);
    monitor.workspaces[target_ws].windows.push_back(moved_window);
    monitor.workspaces[target_ws].focused_window = window_to_move;

    // Update EWMH desktop for moved window
    uint32_t desktop = get_ewmh_desktop_index(focused_monitor_, target_ws);
    ewmh_.set_window_desktop(window_to_move, desktop);

    wm_unmap_window(window_to_move);
    rearrange_monitor(monitor);

    if (!current_ws.windows.empty())
    {
        focus_window(current_ws.windows.back().id);
    }
    else
    {
        clear_focus();
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

    size_t monitor_idx = 0;
    for (; monitor_idx < monitors_.size(); ++monitor_idx)
    {
        if (&monitors_[monitor_idx] == &monitor)
            break;
    }
    if (monitor_idx >= monitors_.size())
    {
        clear_focus();
        return;
    }

    // Verify focused_window actually exists in the workspace (defensive programming)
    if (ws.focused_window != XCB_NONE && ws.find_window(ws.focused_window) != ws.windows.end()
        && !iconic_windows_.contains(ws.focused_window))
    {
        focus_window(ws.focused_window);
    }
    else
    {
        xcb_window_t candidate = XCB_NONE;
        for (auto it = ws.windows.rbegin(); it != ws.windows.rend(); ++it)
        {
            if (!iconic_windows_.contains(it->id))
            {
                candidate = it->id;
                break;
            }
        }
        if (candidate != XCB_NONE)
        {
            // focused_window was stale or XCB_NONE - focus last visible window
            focus_window(candidate);
        }
        else
        {
            auto it = std::find_if(
                floating_windows_.rbegin(),
                floating_windows_.rend(),
                [&](FloatingWindow const& floating_window) {
                    return floating_window.monitor == monitor_idx
                        && floating_window.workspace == monitor.current_workspace
                        && !iconic_windows_.contains(floating_window.id);
                }
            );
            if (it != floating_windows_.rend())
            {
                focus_floating_window(it->id);
            }
            else
            {
                // No windows - clear focus per EWMH spec
                clear_focus();
            }
        }
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
    if (active_window_ == XCB_NONE)
        return;

    size_t target_idx = wrap_monitor_index(static_cast<int>(focused_monitor_) + direction);
    if (target_idx == focused_monitor_)
        return;

    xcb_window_t window_to_move = active_window_;

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

std::optional<size_t> WindowManager::monitor_index_for_window(xcb_window_t window) const
{
    for (size_t m = 0; m < monitors_.size(); ++m)
    {
        for (auto const& workspace : monitors_[m].workspaces)
        {
            if (workspace.find_window(window) != workspace.windows.end())
                return m;
        }
    }

    for (auto const& floating_window : floating_windows_)
    {
        if (floating_window.id == window)
            return floating_window.monitor;
    }

    return std::nullopt;
}

std::optional<size_t> WindowManager::workspace_index_for_window(xcb_window_t window) const
{
    for (size_t m = 0; m < monitors_.size(); ++m)
    {
        for (size_t w = 0; w < monitors_[m].workspaces.size(); ++w)
        {
            if (monitors_[m].workspaces[w].find_window(window) != monitors_[m].workspaces[w].windows.end())
                return w;
        }
    }

    for (auto const& floating_window : floating_windows_)
    {
        if (floating_window.id == window)
            return floating_window.workspace;
    }

    return std::nullopt;
}

std::optional<xcb_window_t> WindowManager::transient_for_window(xcb_window_t window) const
{
    if (wm_transient_for_ == XCB_NONE)
        return std::nullopt;

    auto cookie = xcb_get_property(conn_.get(), 0, window, wm_transient_for_, XCB_ATOM_WINDOW, 0, 1);
    auto* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);
    if (!reply)
        return std::nullopt;

    std::optional<xcb_window_t> result;
    if (xcb_get_property_value_length(reply) >= static_cast<int>(sizeof(xcb_window_t)))
    {
        auto* value = static_cast<xcb_window_t*>(xcb_get_property_value(reply));
        if (value && *value != XCB_NONE)
            result = *value;
    }

    free(reply);
    return result;
}

bool WindowManager::is_override_redirect_window(xcb_window_t window) const
{
    auto cookie = xcb_get_window_attributes(conn_.get(), window);
    auto* reply = xcb_get_window_attributes_reply(conn_.get(), cookie, nullptr);
    if (!reply)
        return false;

    bool override_redirect = reply->override_redirect;
    free(reply);
    return override_redirect;
}

void WindowManager::update_floating_visibility(size_t monitor_idx)
{
    if (monitor_idx >= monitors_.size())
        return;

    auto& monitor = monitors_[monitor_idx];
    for (auto& floating_window : floating_windows_)
    {
        if (floating_window.monitor != monitor_idx)
            continue;

        if (floating_window.workspace == monitor.current_workspace && !iconic_windows_.contains(floating_window.id))
        {
            xcb_map_window(conn_.get(), floating_window.id);
            wm_unmapped_windows_.erase(floating_window.id);
            if (fullscreen_windows_.contains(floating_window.id))
            {
                apply_fullscreen_if_needed(floating_window.id);
            }
            else
            {
                apply_floating_geometry(floating_window);
            }
        }
        else
        {
            wm_unmap_window(floating_window.id);
        }
    }
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
    int32_t center_x = static_cast<int32_t>(window.geometry.x) + static_cast<int32_t>(window.geometry.width) / 2;
    int32_t center_y = static_cast<int32_t>(window.geometry.y) + static_cast<int32_t>(window.geometry.height) / 2;
    auto new_monitor = focus::monitor_index_at_point(monitors_, static_cast<int16_t>(center_x), static_cast<int16_t>(center_y));
    if (!new_monitor || *new_monitor == window.monitor)
        return;

    window.monitor = *new_monitor;
    window.workspace = monitors_[window.monitor].current_workspace;

    uint32_t desktop = get_ewmh_desktop_index(window.monitor, window.workspace);
    ewmh_.set_window_desktop(window.id, desktop);
}

void WindowManager::apply_floating_geometry(FloatingWindow const& window)
{
    uint32_t values[] = {
        static_cast<uint32_t>(window.geometry.x),
        static_cast<uint32_t>(window.geometry.y),
        window.geometry.width,
        window.geometry.height
    };
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    xcb_configure_window(conn_.get(), window.id, mask, values);
}

void WindowManager::send_configure_notify(xcb_window_t window)
{
    auto geom_cookie = xcb_get_geometry(conn_.get(), window);
    auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
    if (!geom_reply)
        return;

    xcb_configure_notify_event_t ev = {};
    ev.response_type = XCB_CONFIGURE_NOTIFY;
    ev.event = window;
    ev.window = window;
    ev.x = geom_reply->x;
    ev.y = geom_reply->y;
    ev.width = geom_reply->width;
    ev.height = geom_reply->height;
    ev.border_width = geom_reply->border_width;
    ev.above_sibling = XCB_NONE;
    ev.override_redirect = 0;

    xcb_send_event(
        conn_.get(),
        0,
        window,
        XCB_EVENT_MASK_STRUCTURE_NOTIFY,
        reinterpret_cast<char*>(&ev)
    );

    free(geom_reply);
}

void WindowManager::begin_drag(xcb_window_t window, bool resize, int16_t root_x, int16_t root_y)
{
    if (fullscreen_windows_.contains(window))
        return;

    auto* floating_window = find_floating_window(window);
    if (!floating_window)
        return;

    drag_state_.active = true;
    drag_state_.resizing = resize;
    drag_state_.window = window;
    drag_state_.start_root_x = root_x;
    drag_state_.start_root_y = root_y;
    drag_state_.start_geometry = floating_window->geometry;

    xcb_grab_pointer(
        conn_.get(),
        0,
        conn_.screen()->root,
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
        XCB_GRAB_MODE_ASYNC,
        XCB_GRAB_MODE_ASYNC,
        XCB_NONE,
        XCB_NONE,
        XCB_CURRENT_TIME
    );
    conn_.flush();
}

void WindowManager::update_drag(int16_t root_x, int16_t root_y)
{
    if (!drag_state_.active)
        return;

    auto* floating_window = find_floating_window(drag_state_.window);
    if (!floating_window)
        return;

    int32_t dx = static_cast<int32_t>(root_x) - static_cast<int32_t>(drag_state_.start_root_x);
    int32_t dy = static_cast<int32_t>(root_y) - static_cast<int32_t>(drag_state_.start_root_y);

    Geometry updated = drag_state_.start_geometry;
    if (drag_state_.resizing)
    {
        int32_t new_w = static_cast<int32_t>(drag_state_.start_geometry.width) + dx;
        int32_t new_h = static_cast<int32_t>(drag_state_.start_geometry.height) + dy;
        updated.width = static_cast<uint16_t>(std::max<int32_t>(1, new_w));
        updated.height = static_cast<uint16_t>(std::max<int32_t>(1, new_h));
    }
    else
    {
        updated.x = static_cast<int16_t>(static_cast<int32_t>(drag_state_.start_geometry.x) + dx);
        updated.y = static_cast<int16_t>(static_cast<int32_t>(drag_state_.start_geometry.y) + dy);
    }

    floating_window->geometry = updated;
    apply_floating_geometry(*floating_window);
    update_floating_monitor_for_geometry(*floating_window);

    if (active_window_ == floating_window->id)
    {
        focused_monitor_ = floating_window->monitor;
        update_ewmh_current_desktop();
        update_all_bars();
    }

    conn_.flush();
}

void WindowManager::end_drag()
{
    if (!drag_state_.active)
        return;

    drag_state_.active = false;
    drag_state_.window = XCB_NONE;
    xcb_ungrab_pointer(conn_.get(), XCB_CURRENT_TIME);
    conn_.flush();
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

void WindowManager::update_focused_monitor_at_point(int16_t x, int16_t y)
{
    auto result = focus::pointer_move(monitors_, focused_monitor_, x, y);
    if (!result.active_monitor_changed)
        return;

    // We crossed monitors - update active monitor and clear focus
    focused_monitor_ = result.new_monitor;
    update_ewmh_current_desktop();
    if (result.clear_focus)
        clear_focus();

    update_all_bars();
    conn_.flush();
}

std::string WindowManager::get_window_name(xcb_window_t window)
{
    if (utf8_string_ != XCB_NONE)
    {
        auto cookie =
            xcb_get_property(conn_.get(), 0, window, ewmh_.get()->_NET_WM_NAME, utf8_string_, 0, 1024);
        auto* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);
        if (reply)
        {
            int len = xcb_get_property_value_length(reply);
            if (len > 0)
            {
                char* name = static_cast<char*>(xcb_get_property_value(reply));
                std::string windowName(name, len);
                free(reply);
                return windowName;
            }
            free(reply);
        }
    }

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

void WindowManager::update_window_title(xcb_window_t window)
{
    std::string name = get_window_name(window);
    bool update_bars = false;

    for (auto& monitor : monitors_)
    {
        for (auto& workspace : monitor.workspaces)
        {
            auto it = workspace.find_window(window);
            if (it != workspace.windows.end())
            {
                if (it->name != name)
                {
                    it->name = name;
                    if (&workspace == &monitor.current())
                        update_bars = true;
                }
                break;
            }
        }
    }

    if (update_bars)
    {
        update_all_bars();
        conn_.flush();
    }
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
    update_ewmh_desktops();
    update_ewmh_current_desktop();
}

void WindowManager::update_ewmh_desktops()
{
    size_t workspaces_per_monitor = config_.workspaces.count;
    uint32_t total_desktops = static_cast<uint32_t>(monitors_.size() * workspaces_per_monitor);
    ewmh_.set_number_of_desktops(total_desktops);

    std::vector<std::string> names;
    names.reserve(total_desktops);
    for (size_t m = 0; m < monitors_.size(); ++m)
    {
        for (size_t w = 0; w < workspaces_per_monitor; ++w)
        {
            if (w < config_.workspaces.names.size())
            {
                names.push_back(config_.workspaces.names[w]);
            }
            else
            {
                names.push_back(std::to_string(w + 1));
            }
        }
    }
    ewmh_.set_desktop_names(names);
    ewmh_.set_desktop_viewport(monitors_);
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
    for (auto const& floating_window : floating_windows_)
    {
        windows.push_back(floating_window.id);
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
    return static_cast<uint32_t>(monitor_idx * config_.workspaces.count + workspace_idx);
}

void WindowManager::switch_to_ewmh_desktop(uint32_t desktop)
{
    // Convert EWMH desktop index to monitor + workspace
    size_t workspaces_per_monitor = config_.workspaces.count;
    if (workspaces_per_monitor == 0)
        return;

    size_t monitor_idx = desktop / workspaces_per_monitor;
    size_t workspace_idx = desktop % workspaces_per_monitor;

    if (monitor_idx >= monitors_.size())
        return;

    auto& monitor = monitors_[monitor_idx];
    if (workspace_idx >= monitor.workspaces.size())
        return;

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
    update_floating_visibility(monitor_idx);
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
    for (auto const& floating_window : floating_windows_)
    {
        xcb_change_window_attributes(
            conn_.get(),
            floating_window.id,
            XCB_CW_BORDER_PIXEL,
            &conn_.screen()->black_pixel
        );
    }
    conn_.flush();
}

xcb_atom_t WindowManager::intern_atom(char const* name) const
{
    auto cookie = xcb_intern_atom(conn_.get(), 0, static_cast<uint16_t>(strlen(name)), name);
    auto* reply = xcb_intern_atom_reply(conn_.get(), cookie, nullptr);
    if (!reply)
        return XCB_NONE;

    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

} // namespace lwm
