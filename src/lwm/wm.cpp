#include "wm.hpp"
#include "lwm/core/floating.hpp"
#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb_icccm.h>

namespace lwm {

namespace {

constexpr uint32_t WM_STATE_WITHDRAWN = 0;
constexpr uint32_t WM_STATE_NORMAL = 1;
constexpr uint32_t WM_STATE_ICONIC = 3;
constexpr auto PING_TIMEOUT = std::chrono::seconds(5);
constexpr auto KILL_TIMEOUT = std::chrono::seconds(5);
constexpr auto SYNC_WAIT_TIMEOUT = std::chrono::milliseconds(200);

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

uint32_t extract_event_time(uint8_t response_type, xcb_generic_event_t const& event)
{
    switch (response_type)
    {
        case XCB_KEY_PRESS:
        case XCB_KEY_RELEASE:
            return reinterpret_cast<xcb_key_press_event_t const&>(event).time;
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE:
            return reinterpret_cast<xcb_button_press_event_t const&>(event).time;
        case XCB_MOTION_NOTIFY:
            return reinterpret_cast<xcb_motion_notify_event_t const&>(event).time;
        case XCB_ENTER_NOTIFY:
        case XCB_LEAVE_NOTIFY:
            return reinterpret_cast<xcb_enter_notify_event_t const&>(event).time;
        case XCB_PROPERTY_NOTIFY:
            return reinterpret_cast<xcb_property_notify_event_t const&>(event).time;
        default:
            return 0;
    }
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
    init_mousebinds();
    create_wm_window();
    setup_root();
    grab_buttons();
    claim_wm_ownership();
    wm_transient_for_ = intern_atom("WM_TRANSIENT_FOR");
    wm_state_ = intern_atom("WM_STATE");
    wm_change_state_ = intern_atom("WM_CHANGE_STATE");
    utf8_string_ = intern_atom("UTF8_STRING");
    wm_protocols_ = intern_atom("WM_PROTOCOLS");
    wm_delete_window_ = intern_atom("WM_DELETE_WINDOW");
    wm_take_focus_ = intern_atom("WM_TAKE_FOCUS");
    wm_normal_hints_ = intern_atom("WM_NORMAL_HINTS");
    wm_hints_ = intern_atom("WM_HINTS");
    net_wm_ping_ = ewmh_.get()->_NET_WM_PING;
    net_wm_sync_request_ = ewmh_.get()->_NET_WM_SYNC_REQUEST;
    net_wm_sync_request_counter_ = ewmh_.get()->_NET_WM_SYNC_REQUEST_COUNTER;
    net_close_window_ = ewmh_.get()->_NET_CLOSE_WINDOW;
    net_wm_fullscreen_monitors_ = ewmh_.get()->_NET_WM_FULLSCREEN_MONITORS;
    net_wm_user_time_ = ewmh_.get()->_NET_WM_USER_TIME;
    layout_.set_sync_request_callback([this](xcb_window_t window) { send_sync_request(window, last_event_time_); });
    detect_monitors();
    setup_ewmh();
    if (bar_)
    {
        setup_monitor_bars();
    }
    scan_existing_windows();
    keybinds_.grab_keys(conn_.screen()->root);
    update_ewmh_client_list();
    update_all_bars();
    conn_.flush();
}

void WindowManager::run()
{
    int fd = xcb_get_file_descriptor(conn_.get());
    pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;

    while (running_)
    {
        int timeout_ms = -1;
        auto now = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> next_deadline;

        for (auto const& [window, deadline] : pending_kills_)
        {
            if (!next_deadline || deadline < *next_deadline)
                next_deadline = deadline;
        }
        for (auto const& [window, deadline] : pending_pings_)
        {
            if (!next_deadline || deadline < *next_deadline)
                next_deadline = deadline;
        }

        if (next_deadline)
        {
            if (*next_deadline <= now)
            {
                timeout_ms = 0;
            }
            else
            {
                auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(*next_deadline - now);
                timeout_ms = static_cast<int>(delta.count());
            }
        }

        int poll_result = poll(&pfd, 1, timeout_ms);
        if (poll_result > 0)
        {
            while (auto event = xcb_poll_for_event(conn_.get()))
            {
                std::unique_ptr<xcb_generic_event_t, decltype(&free)> eventPtr(event, free);
                handle_event(*eventPtr);
            }
        }

        handle_timeouts();

        if (xcb_connection_has_error(conn_.get()))
            break;
    }
}

void WindowManager::setup_root()
{
    uint32_t values[] = { XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                          | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_POINTER_MOTION
                          | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE
                          | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE };
    auto cookie = xcb_change_window_attributes_checked(conn_.get(), conn_.screen()->root, XCB_CW_EVENT_MASK, values);
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

void WindowManager::init_mousebinds()
{
    mousebinds_.clear();
    mousebinds_.reserve(config_.mousebinds.size());

    for (auto const& mb : config_.mousebinds)
    {
        if (mb.button <= 0 || mb.button > std::numeric_limits<uint8_t>::max())
            continue;
        if (mb.action.empty())
            continue;

        MouseBinding binding;
        binding.modifier = KeybindManager::parse_modifier(mb.mod);
        binding.button = static_cast<uint8_t>(mb.button);
        binding.action = mb.action;
        mousebinds_.push_back(std::move(binding));
    }
}

void WindowManager::grab_buttons()
{
    xcb_window_t root = conn_.screen()->root;
    xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, root, XCB_MOD_MASK_ANY);

    for (auto const& binding : mousebinds_)
    {
        uint16_t modifiers[] = {
            binding.modifier,
            static_cast<uint16_t>(binding.modifier | XCB_MOD_MASK_2),
            static_cast<uint16_t>(binding.modifier | XCB_MOD_MASK_LOCK),
            static_cast<uint16_t>(binding.modifier | XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK)
        };

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
                binding.button,
                mod
            );
        }
    }

    conn_.flush();
}

void WindowManager::claim_wm_ownership()
{
    wm_s0_ = intern_atom("WM_S0");
    if (wm_s0_ == XCB_NONE)
        throw std::runtime_error("Failed to intern WM_S0 atom");

    auto owner_cookie = xcb_get_selection_owner(conn_.get(), wm_s0_);
    auto* owner_reply = xcb_get_selection_owner_reply(conn_.get(), owner_cookie, nullptr);
    if (!owner_reply)
        throw std::runtime_error("Failed to query WM selection owner");

    if (owner_reply->owner != XCB_NONE)
    {
        free(owner_reply);
        throw std::runtime_error("Another window manager already owns WM_S0");
    }
    free(owner_reply);

    xcb_set_selection_owner(conn_.get(), wm_window_, wm_s0_, XCB_CURRENT_TIME);

    owner_cookie = xcb_get_selection_owner(conn_.get(), wm_s0_);
    owner_reply = xcb_get_selection_owner_reply(conn_.get(), owner_cookie, nullptr);
    if (!owner_reply || owner_reply->owner != wm_window_)
    {
        if (owner_reply)
            free(owner_reply);
        throw std::runtime_error("Failed to acquire WM_S0 selection");
    }
    free(owner_reply);

    // Broadcast MANAGER client message to notify clients that a new WM has started (ICCCM)
    xcb_atom_t manager_atom = intern_atom("MANAGER");
    if (manager_atom != XCB_NONE)
    {
        xcb_client_message_event_t event = {};
        event.response_type = XCB_CLIENT_MESSAGE;
        event.format = 32;
        event.window = conn_.screen()->root;
        event.type = manager_atom;
        event.data.data32[0] = XCB_CURRENT_TIME;
        event.data.data32[1] = wm_s0_;
        event.data.data32[2] = wm_window_;
        event.data.data32[3] = 0;
        event.data.data32[4] = 0;

        xcb_send_event(
            conn_.get(),
            0,
            conn_.screen()->root,
            XCB_EVENT_MASK_STRUCTURE_NOTIFY,
            reinterpret_cast<char const*>(&event)
        );
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
    monitor.previous_workspace = 0;
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
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_POPUP_MENU || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_TOOLTIP
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_NOTIFICATION || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_COMBO
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DND;
        bool is_floating_type = type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DIALOG
            || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_UTILITY || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_SPLASH;
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
    uint32_t event_time = extract_event_time(response_type, event);
    if (event_time != 0)
        last_event_time_ = event_time;

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
        case XCB_EXPOSE:
            handle_expose(reinterpret_cast<xcb_expose_event_t const&>(event));
            break;
        case XCB_SELECTION_CLEAR:
        {
            auto const& e = reinterpret_cast<xcb_selection_clear_event_t const&>(event);
            // Another WM is taking over - exit gracefully (ICCCM)
            if (e.selection == wm_s0_)
            {
                running_ = false;
            }
            break;
        }
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
            focus = *monitor_idx == focused_monitor_ && *workspace_idx == monitors_[*monitor_idx].current_workspace;
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
        if (std::ranges::find(dock_windows_, e.window) == dock_windows_.end())
        {
            dock_windows_.push_back(e.window);
        }
        update_struts();
        rearrange_all_monitors();
        conn_.flush();
        return;
    }

    if (is_override_redirect_window(e.window))
    {
        return;
    }

    xcb_atom_t type = ewmh_.get_window_type(e.window);
    bool is_menu = type == ewmh_.get()->_NET_WM_WINDOW_TYPE_MENU
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DROPDOWN_MENU || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_POPUP_MENU
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_TOOLTIP || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_NOTIFICATION
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_COMBO || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DND;
    bool is_floating_type = type == ewmh_.get()->_NET_WM_WINDOW_TYPE_DIALOG
        || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_UTILITY || type == ewmh_.get()->_NET_WM_WINDOW_TYPE_SPLASH;
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
        // Check WM_HINTS initial_state for floating windows (ICCCM)
        bool start_iconic = false;
        xcb_icccm_wm_hints_t hints;
        if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), e.window), &hints, nullptr))
        {
            if ((hints.flags & XCB_ICCCM_WM_HINT_STATE) && hints.initial_state == XCB_ICCCM_WM_STATE_ICONIC)
            {
                start_iconic = true;
            }
        }
        manage_floating_window(e.window, start_iconic);
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

    // Check WM_HINTS initial_state (ICCCM)
    bool start_iconic = false;
    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), e.window), &hints, nullptr))
    {
        if ((hints.flags & XCB_ICCCM_WM_HINT_STATE) && hints.initial_state == XCB_ICCCM_WM_STATE_ICONIC)
        {
            start_iconic = true;
        }
    }

    manage_window(e.window, start_iconic);
    if (!start_iconic)
    {
        auto monitor_idx = monitor_index_for_window(e.window);
        auto workspace_idx = workspace_index_for_window(e.window);
        if (monitor_idx && workspace_idx && *monitor_idx == focused_monitor_
            && *workspace_idx == monitors_[*monitor_idx].current_workspace)
        {
            // Window already mapped by layout_.arrange() in manage_window()
            focus_window(e.window);
        }
    }
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

WindowManager::MouseBinding const* WindowManager::resolve_mouse_binding(uint16_t state, uint8_t button) const
{
    uint16_t clean_mod = state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2);
    for (auto const& binding : mousebinds_)
    {
        if (binding.button == button && binding.modifier == clean_mod)
            return &binding;
    }
    return nullptr;
}

void WindowManager::handle_button_press(xcb_button_press_event_t const& e)
{
    xcb_window_t target = e.event;
    if (target == conn_.screen()->root && e.child != XCB_NONE)
        target = e.child;

    if (auto const* binding = resolve_mouse_binding(e.state, e.detail))
    {
        if (binding->action == "drag_window")
        {
            if (find_floating_window(target))
            {
                focus_floating_window(target);
                begin_drag(target, false, e.root_x, e.root_y);
                return;
            }
            if (monitor_containing_window(target))
            {
                focus_window(target);
                begin_tiled_drag(target, e.root_x, e.root_y);
                return;
            }
        }
        else if (binding->action == "resize_floating")
        {
            if (find_floating_window(target))
            {
                focus_floating_window(target);
                begin_drag(target, true, e.root_x, e.root_y);
                return;
            }
        }
    }

    // Only handle clicks on root window (empty areas or gaps)
    if (e.event != conn_.screen()->root)
        return;

    // Update focused monitor based on click position
    update_focused_monitor_at_point(e.root_x, e.root_y);
}

void WindowManager::handle_button_release(xcb_button_release_event_t const& e)
{
    if (!drag_state_.active)
        return;

    drag_state_.last_root_x = e.root_x;
    drag_state_.last_root_y = e.root_y;
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
    else if (action->type == "toggle_workspace")
    {
        toggle_workspace();
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

    if (e.type == wm_protocols_ && e.data.data32[0] == net_wm_ping_)
    {
        xcb_window_t window = static_cast<xcb_window_t>(e.data.data32[2]);
        if (window == XCB_NONE)
        {
            window = e.window;
        }
        pending_pings_.erase(window);
        return;
    }

    if (e.type == net_close_window_)
    {
        kill_window(e.window);
        return;
    }

    if (e.type == net_wm_fullscreen_monitors_)
    {
        FullscreenMonitors monitors;
        monitors.top = e.data.data32[0];
        monitors.bottom = e.data.data32[1];
        monitors.left = e.data.data32[2];
        monitors.right = e.data.data32[3];
        set_fullscreen_monitors(e.window, monitors);
        return;
    }

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

        auto handle_state = [&](xcb_atom_t state)
        {
            if (state == XCB_ATOM_NONE)
                return;

            // Helper to compute enable state based on action
            auto compute_enable = [action](bool currently_set)
            {
                if (action == 0)
                    return false; // Remove
                if (action == 1)
                    return true;       // Add
                return !currently_set; // Toggle
            };

            if (state == ewmh->_NET_WM_STATE_FULLSCREEN)
            {
                bool enable = compute_enable(fullscreen_windows_.contains(e.window));
                set_fullscreen(e.window, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_ABOVE)
            {
                bool enable = compute_enable(above_windows_.contains(e.window));
                set_window_above(e.window, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_BELOW)
            {
                bool enable = compute_enable(below_windows_.contains(e.window));
                set_window_below(e.window, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_SKIP_TASKBAR)
            {
                bool enable = compute_enable(skip_taskbar_windows_.contains(e.window));
                if (enable)
                    skip_taskbar_windows_.insert(e.window);
                else
                    skip_taskbar_windows_.erase(e.window);
                ewmh_.set_window_state(e.window, ewmh->_NET_WM_STATE_SKIP_TASKBAR, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_SKIP_PAGER)
            {
                bool enable = compute_enable(skip_pager_windows_.contains(e.window));
                if (enable)
                    skip_pager_windows_.insert(e.window);
                else
                    skip_pager_windows_.erase(e.window);
                ewmh_.set_window_state(e.window, ewmh->_NET_WM_STATE_SKIP_PAGER, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION)
            {
                bool enable = compute_enable(ewmh_.has_urgent_hint(e.window));
                ewmh_.set_demands_attention(e.window, enable);
                update_all_bars();
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
        uint32_t source = e.data.data32[0]; // 1 = application, 2 = pager
        uint32_t timestamp = e.data.data32[1];
        LWM_DEBUG("_NET_ACTIVE_WINDOW request: window=0x" << std::hex << window << std::dec << " source=" << source);

        // Focus stealing prevention: for application-initiated requests (source=1),
        // check if the request timestamp is newer than the current active window's user time
        if (source == 1 && active_window_ != XCB_NONE && timestamp != 0)
        {
            auto active_time_it = user_times_.find(active_window_);
            if (active_time_it != user_times_.end() && active_time_it->second != 0)
            {
                // If the requesting window's timestamp is older than the active window's last
                // user interaction, set demands attention instead of stealing focus
                if (timestamp < active_time_it->second)
                {
                    LWM_DEBUG("Focus stealing prevented, setting demands attention");
                    ewmh_.set_demands_attention(window, true);
                    update_all_bars();
                    return;
                }
            }
        }

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
    // Handle _NET_WM_DESKTOP requests (move window to another desktop)
    else if (e.type == ewmh->_NET_WM_DESKTOP)
    {
        uint32_t desktop = e.data.data32[0];
        LWM_DEBUG("_NET_WM_DESKTOP request: window=0x" << std::hex << e.window << std::dec << " desktop=" << desktop);

        // 0xFFFFFFFF means sticky (visible on all desktops) - not implemented, ignore
        if (desktop == 0xFFFFFFFF)
            return;

        // Calculate target monitor and workspace from desktop index
        size_t workspaces_per_monitor = config_.workspaces.count;
        size_t target_monitor = desktop / workspaces_per_monitor;
        size_t target_workspace = desktop % workspaces_per_monitor;

        if (target_monitor >= monitors_.size())
            return;

        bool was_active = (active_window_ == e.window);
        auto target_visible = [&](size_t monitor_idx, size_t workspace_idx)
        {
            if (showing_desktop_)
                return false;
            return monitor_idx < monitors_.size() && workspace_idx == monitors_[monitor_idx].current_workspace;
        };

        // Handle tiled windows
        if (auto* source_monitor = monitor_containing_window(e.window))
        {
            auto source_idx = workspace_index_for_window(e.window);
            if (!source_idx)
                return;

            auto& source_ws = source_monitor->workspaces[*source_idx];
            auto it = source_ws.find_window(e.window);
            if (it == source_ws.windows.end())
                return;

            Window win = *it;
            source_ws.windows.erase(it);
            if (source_ws.focused_window == e.window)
            {
                source_ws.focused_window = XCB_NONE;
                for (auto rit = source_ws.windows.rbegin(); rit != source_ws.windows.rend(); ++rit)
                {
                    if (!iconic_windows_.contains(rit->id))
                    {
                        source_ws.focused_window = rit->id;
                        break;
                    }
                }
            }

            auto& target_ws = monitors_[target_monitor].workspaces[target_workspace];
            target_ws.windows.push_back(win);
            if (!target_visible(target_monitor, target_workspace) || was_active)
            {
                target_ws.focused_window = e.window;
            }
            ewmh_.set_window_desktop(e.window, desktop);

            rearrange_monitor(*source_monitor);
            if (source_monitor != &monitors_[target_monitor])
            {
                rearrange_monitor(monitors_[target_monitor]);
            }

            // Hide if moved to non-current workspace
            if (!target_visible(target_monitor, target_workspace))
            {
                wm_unmap_window(e.window);
            }

            update_ewmh_client_list();
            if (was_active && !target_visible(target_monitor, target_workspace))
            {
                focus_or_fallback(focused_monitor());
            }
            update_all_bars();
            conn_.flush();
        }
        // Handle floating windows
        else if (auto* fw = find_floating_window(e.window))
        {
            size_t source_monitor = fw->monitor;
            fw->monitor = target_monitor;
            fw->workspace = target_workspace;
            if (source_monitor != target_monitor)
            {
                fw->geometry = floating::place_floating(
                    monitors_[target_monitor].working_area(),
                    fw->geometry.width,
                    fw->geometry.height,
                    std::nullopt
                );
            }
            ewmh_.set_window_desktop(e.window, desktop);

            if (source_monitor != target_monitor)
            {
                update_floating_visibility(source_monitor);
            }
            update_floating_visibility(target_monitor);
            update_ewmh_client_list();
            if (was_active && !target_visible(target_monitor, target_workspace))
            {
                focus_or_fallback(focused_monitor());
            }
            update_all_bars();
            conn_.flush();
        }
    }
    // Handle _NET_REQUEST_FRAME_EXTENTS (apps query this before mapping)
    else if (e.type == ewmh->_NET_REQUEST_FRAME_EXTENTS)
    {
        // LWM doesn't add frames, so extents are all zeros
        ewmh_.set_frame_extents(e.window, 0, 0, 0, 0);
        conn_.flush();
    }
    // Handle _NET_MOVERESIZE_WINDOW (programmatic move/resize for floating windows)
    else if (e.type == ewmh->_NET_MOVERESIZE_WINDOW)
    {
        auto* fw = find_floating_window(e.window);
        if (!fw)
            return; // Only floating windows support this

        uint32_t flags = e.data.data32[0];
        // flags: bits 8-11 = gravity, bit 12 = x, bit 13 = y, bit 14 = width, bit 15 = height
        bool has_x = flags & (1 << 8);
        bool has_y = flags & (1 << 9);
        bool has_width = flags & (1 << 10);
        bool has_height = flags & (1 << 11);

        if (has_x)
            fw->geometry.x = static_cast<int16_t>(e.data.data32[1]);
        if (has_y)
            fw->geometry.y = static_cast<int16_t>(e.data.data32[2]);
        if (has_width)
            fw->geometry.width = static_cast<uint16_t>(std::max<int32_t>(1, e.data.data32[3]));
        if (has_height)
            fw->geometry.height = static_cast<uint16_t>(std::max<int32_t>(1, e.data.data32[4]));

        update_floating_monitor_for_geometry(*fw);
        if (fw->workspace == monitors_[fw->monitor].current_workspace && !iconic_windows_.contains(fw->id)
            && !fullscreen_windows_.contains(fw->id))
        {
            apply_floating_geometry(*fw);
        }
        conn_.flush();
    }
    // Handle _NET_WM_MOVERESIZE (interactive move/resize initiated by application)
    else if (e.type == ewmh->_NET_WM_MOVERESIZE)
    {
        auto* fw = find_floating_window(e.window);
        if (!fw)
            return; // Only floating windows support this

        int16_t x_root = static_cast<int16_t>(e.data.data32[0]);
        int16_t y_root = static_cast<int16_t>(e.data.data32[1]);
        uint32_t direction = e.data.data32[2];

        // Direction: 8 = move, 11 = cancel, 0-7 = resize edges
        if (direction == 11)
        {
            // Cancel
            end_drag();
        }
        else if (direction == 8)
        {
            // Move
            focus_floating_window(e.window);
            begin_drag(e.window, false, x_root, y_root);
        }
        else if (direction <= 7)
        {
            // Resize (direction indicates edge/corner)
            focus_floating_window(e.window);
            begin_drag(e.window, true, x_root, y_root);
        }
    }
    // Handle _NET_SHOWING_DESKTOP (show desktop mode toggle)
    else if (e.type == ewmh->_NET_SHOWING_DESKTOP)
    {
        bool show = e.data.data32[0] != 0;
        if (show != showing_desktop_)
        {
            showing_desktop_ = show;
            ewmh_.set_showing_desktop(showing_desktop_);

            if (showing_desktop_)
            {
                // Hide all windows
                for (auto const& monitor : monitors_)
                {
                    for (auto const& window : monitor.current().windows)
                    {
                        wm_unmap_window(window.id);
                    }
                }
                for (auto const& fw : floating_windows_)
                {
                    if (fw.monitor < monitors_.size() && fw.workspace == monitors_[fw.monitor].current_workspace)
                    {
                        wm_unmap_window(fw.id);
                    }
                }
                clear_focus();
            }
            else
            {
                // Show windows again
                rearrange_all_monitors();
                update_floating_visibility_all();
                if (!monitors_.empty())
                {
                    focus_or_fallback(focused_monitor());
                }
            }
            conn_.flush();
        }
    }
    // Handle _NET_RESTACK_WINDOW (restack window relative to sibling)
    else if (e.type == ewmh->_NET_RESTACK_WINDOW)
    {
        xcb_window_t sibling = static_cast<xcb_window_t>(e.data.data32[1]);
        uint32_t detail = e.data.data32[2];

        uint32_t values[2];
        uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;
        values[0] = detail; // Stack mode

        if (sibling != XCB_NONE)
        {
            mask |= XCB_CONFIG_WINDOW_SIBLING;
            values[0] = sibling;
            values[1] = detail;
        }

        xcb_configure_window(conn_.get(), e.window, mask, values);
        conn_.flush();
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

        uint16_t requested_width = floating_window->geometry.width;
        uint16_t requested_height = floating_window->geometry.height;
        uint32_t hinted_width = requested_width;
        uint32_t hinted_height = requested_height;
        layout_.apply_size_hints(floating_window->id, hinted_width, hinted_height);
        floating_window->geometry.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
        floating_window->geometry.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));

        if (requested_width != floating_window->geometry.width || requested_height != floating_window->geometry.height)
        {
            apply_floating_geometry(*floating_window);
        }

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
    if (wm_normal_hints_ != XCB_NONE && e.atom == wm_normal_hints_)
    {
        if (auto* floating_window = find_floating_window(e.window))
        {
            uint32_t hinted_width = floating_window->geometry.width;
            uint32_t hinted_height = floating_window->geometry.height;
            layout_.apply_size_hints(floating_window->id, hinted_width, hinted_height);
            floating_window->geometry.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
            floating_window->geometry.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));
            if (floating_window->workspace == monitors_[floating_window->monitor].current_workspace
                && !iconic_windows_.contains(floating_window->id) && !fullscreen_windows_.contains(floating_window->id))
            {
                apply_floating_geometry(*floating_window);
            }
        }
        else if (monitor_containing_window(e.window))
        {
            if (auto monitor_idx = monitor_index_for_window(e.window))
            {
                rearrange_monitor(monitors_[*monitor_idx]);
            }
        }
    }
    if ((wm_protocols_ != XCB_NONE && e.atom == wm_protocols_)
        || (net_wm_sync_request_counter_ != XCB_NONE && e.atom == net_wm_sync_request_counter_))
    {
        update_sync_state(e.window);
    }
    if (net_wm_fullscreen_monitors_ != XCB_NONE && e.atom == net_wm_fullscreen_monitors_)
    {
        update_fullscreen_monitor_state(e.window);
        if (fullscreen_windows_.contains(e.window))
        {
            apply_fullscreen_if_needed(e.window);
        }
    }
    if (net_wm_user_time_ != XCB_NONE && e.atom == net_wm_user_time_)
    {
        user_times_[e.window] = get_user_time(e.window);
    }
}

void WindowManager::handle_expose(xcb_expose_event_t const& e)
{
    if (!bar_ || e.count != 0)
        return;

    auto active_info = get_active_window_info();
    for (size_t i = 0; i < monitors_.size(); ++i)
    {
        auto const& monitor = monitors_[i];
        if (monitor.bar_window == e.window)
        {
            bar_->update(monitor, build_bar_state(i, active_info));
            break;
        }
    }
}

void WindowManager::handle_timeouts()
{
    auto now = std::chrono::steady_clock::now();

    for (auto it = pending_pings_.begin(); it != pending_pings_.end();)
    {
        if (it->second <= now)
        {
            it = pending_pings_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = pending_kills_.begin(); it != pending_kills_.end();)
    {
        if (it->second <= now)
        {
            xcb_kill_client(conn_.get(), it->first);
            it = pending_kills_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    conn_.flush();
}

void WindowManager::handle_randr_screen_change()
{
    // Exit fullscreen for all windows before reconfiguring monitors
    // This prevents stale geometry in fullscreen_restore_ after monitor layout changes
    std::vector<xcb_window_t> fullscreen_copy(fullscreen_windows_.begin(), fullscreen_windows_.end());
    for (auto window : fullscreen_copy)
    {
        set_fullscreen(window, false);
    }

    std::vector<Window> all_windows;
    std::vector<FloatingWindow> all_floating = floating_windows_;
    fullscreen_monitors_.clear();
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
    update_struts();

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

void WindowManager::manage_window(xcb_window_t window, bool start_iconic)
{
    auto [instance_name, class_name] = get_wm_class(window);
    Window newWindow = { window, get_window_name(window), class_name, instance_name };
    auto target = resolve_window_desktop(window);
    size_t target_monitor_idx = target ? target->first : focused_monitor_;
    size_t target_workspace_idx =
        target ? target->second : monitors_[target_monitor_idx].current_workspace;

    monitors_[target_monitor_idx].workspaces[target_workspace_idx].windows.push_back(newWindow);
    user_times_[window] = get_user_time(window);

    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
                          | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);

    // Set border width BEFORE layout so positions are calculated correctly
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { start_iconic ? WM_STATE_ICONIC : WM_STATE_NORMAL, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    if (start_iconic)
    {
        iconic_windows_.insert(window);
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_HIDDEN, true);
    }

    update_sync_state(window);
    update_fullscreen_monitor_state(window);

    // Set EWMH properties
    ewmh_.set_frame_extents(window, 0, 0, 0, 0); // LWM doesn't add frames
    uint32_t desktop = get_ewmh_desktop_index(target_monitor_idx, target_workspace_idx);
    ewmh_.set_window_desktop(window, desktop);

    // Set allowed actions for tiled windows (no move/resize via EWMH)
    xcb_ewmh_connection_t* ewmh = ewmh_.get();
    std::vector<xcb_atom_t> actions = {
        ewmh->_NET_WM_ACTION_CLOSE, ewmh->_NET_WM_ACTION_FULLSCREEN, ewmh->_NET_WM_ACTION_CHANGE_DESKTOP,
        ewmh->_NET_WM_ACTION_ABOVE, ewmh->_NET_WM_ACTION_BELOW, ewmh->_NET_WM_ACTION_MINIMIZE,
    };
    ewmh_.set_allowed_actions(window, actions);

    update_ewmh_client_list();

    keybinds_.grab_keys(window);
    if (!start_iconic)
    {
        if (is_workspace_visible(target_monitor_idx, target_workspace_idx))
        {
            rearrange_monitor(monitors_[target_monitor_idx]);
        }
        else
        {
            auto attr_cookie = xcb_get_window_attributes(conn_.get(), window);
            auto* attr_reply = xcb_get_window_attributes_reply(conn_.get(), attr_cookie, nullptr);
            if (attr_reply)
            {
                bool viewable = attr_reply->map_state == XCB_MAP_STATE_VIEWABLE;
                free(attr_reply);
                if (viewable)
                    wm_unmap_window(window);
            }
            else
            {
                wm_unmap_window(window);
            }
        }
    }
    update_all_bars();

    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_TASKBAR))
        skip_taskbar_windows_.insert(window);
    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_PAGER))
        skip_pager_windows_.insert(window);

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

    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN))
    {
        set_fullscreen(window, true);
    }
}

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
    floating_window.monitor = *monitor_idx;
    floating_window.workspace = *workspace_idx;
    floating_window.geometry = placement;
    floating_window.name = get_window_name(window);
    floating_windows_.push_back(floating_window);
    user_times_[window] = get_user_time(window);

    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
                          | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { start_iconic ? WM_STATE_ICONIC : WM_STATE_NORMAL, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    if (start_iconic)
    {
        iconic_windows_.insert(window);
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
        ewmh->_NET_WM_ACTION_MOVE,
        ewmh->_NET_WM_ACTION_RESIZE,
    };
    ewmh_.set_allowed_actions(window, actions);

    update_ewmh_client_list();

    keybinds_.grab_keys(window);
    if (!start_iconic)
    {
        update_floating_visibility(*monitor_idx);
        if (!suppress_focus_ && *monitor_idx == focused_monitor_ && is_workspace_visible(*monitor_idx, *workspace_idx))
            focus_floating_window(window);
    }

    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_TASKBAR))
        skip_taskbar_windows_.insert(window);
    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_PAGER))
        skip_pager_windows_.insert(window);

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

    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN))
    {
        set_fullscreen(window, true);
    }
}

void WindowManager::unmanage_window(xcb_window_t window)
{
    // Set WM_STATE to Withdrawn before unmanaging (ICCCM)
    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_WITHDRAWN, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    fullscreen_windows_.erase(window);
    fullscreen_restore_.erase(window);
    above_windows_.erase(window);
    below_windows_.erase(window);
    iconic_windows_.erase(window);
    skip_taskbar_windows_.erase(window);
    skip_pager_windows_.erase(window);
    fullscreen_monitors_.erase(window);
    sync_counters_.erase(window);
    sync_values_.erase(window);
    pending_kills_.erase(window);
    pending_pings_.erase(window);
    user_times_.erase(window);

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
    // Set WM_STATE to Withdrawn before unmanaging (ICCCM)
    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_WITHDRAWN, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    fullscreen_windows_.erase(window);
    fullscreen_restore_.erase(window);
    above_windows_.erase(window);
    below_windows_.erase(window);
    iconic_windows_.erase(window);
    skip_taskbar_windows_.erase(window);
    skip_pager_windows_.erase(window);
    fullscreen_monitors_.erase(window);
    sync_counters_.erase(window);
    sync_values_.erase(window);
    pending_kills_.erase(window);
    pending_pings_.erase(window);
    user_times_.erase(window);

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
                    [&](FloatingWindow const& floating_window)
                    { return floating_window.monitor == monitor_idx && floating_window.workspace == workspace_idx; }
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
    if (showing_desktop_)
        return;

    if (!is_focus_eligible(window))
        return;

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
    send_wm_take_focus(window, last_event_time_);
    if (should_set_input_focus(window))
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME);
    }
    else
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, XCB_CURRENT_TIME);
    }

    // Clear urgent hint when window receives focus
    ewmh_.set_demands_attention(window, false);
    ewmh_.set_active_window(window);
    user_times_[window] = last_event_time_;

    apply_fullscreen_if_needed(window);

    update_all_bars();
    conn_.flush();
}

void WindowManager::focus_floating_window(xcb_window_t window)
{
    if (showing_desktop_)
        return;

    auto* floating_window = find_floating_window(window);
    if (!floating_window)
        return;

    if (!is_focus_eligible(window))
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
        monitor.previous_workspace = monitor.current_workspace;
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
    send_wm_take_focus(window, last_event_time_);
    if (should_set_input_focus(window))
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME);
    }
    else
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, XCB_CURRENT_TIME);
    }

    uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);

    ewmh_.set_demands_attention(window, false);
    ewmh_.set_active_window(window);
    user_times_[window] = last_event_time_;

    apply_fullscreen_if_needed(window);

    update_ewmh_client_list();
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

        // Fullscreen is incompatible with ABOVE/BELOW states - clear them
        if (above_windows_.contains(window))
        {
            above_windows_.erase(window);
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE, false);
        }
        if (below_windows_.contains(window))
        {
            below_windows_.erase(window);
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_BELOW, false);
        }

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

    update_ewmh_client_list();
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
        if (below_windows_.contains(window))
        {
            below_windows_.erase(window);
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_BELOW, false);
        }
        uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
    }
    else
    {
        above_windows_.erase(window);
    }

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE, enabled);
    update_ewmh_client_list();
    conn_.flush();
}

void WindowManager::set_window_below(xcb_window_t window, bool enabled)
{
    if (!monitor_index_for_window(window))
        return;

    if (enabled)
    {
        below_windows_.insert(window);
        if (above_windows_.contains(window))
        {
            above_windows_.erase(window);
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE, false);
        }
        uint32_t stack_mode = XCB_STACK_MODE_BELOW;
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
    }
    else
    {
        below_windows_.erase(window);
    }

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_BELOW, enabled);
    update_ewmh_client_list();
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
        if (!monitor_idx || *monitor_idx >= monitors_.size())
            return;
        if (floating_window->workspace != monitors_[*monitor_idx].current_workspace)
            return;
    }
    else
    {
        monitor_idx = monitor_index_for_window(window);
        auto workspace_idx = workspace_index_for_window(window);
        if (!monitor_idx || *monitor_idx >= monitors_.size())
            return;
        if (!workspace_idx || *workspace_idx != monitors_[*monitor_idx].current_workspace)
            return;
    }

    Geometry area = fullscreen_geometry_for_window(window);
    uint32_t values[] = { static_cast<uint32_t>(area.x), static_cast<uint32_t>(area.y), area.width, area.height, 0 };
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
        | XCB_CONFIG_WINDOW_BORDER_WIDTH;
    xcb_configure_window(conn_.get(), window, mask, values);

    uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
}

void WindowManager::set_fullscreen_monitors(xcb_window_t window, FullscreenMonitors const& monitors)
{
    fullscreen_monitors_[window] = monitors;

    if (net_wm_fullscreen_monitors_ != XCB_NONE)
    {
        xcb_ewmh_set_wm_fullscreen_monitors(
            ewmh_.get(),
            window,
            monitors.top,
            monitors.bottom,
            monitors.left,
            monitors.right
        );
    }

    if (fullscreen_windows_.contains(window))
    {
        apply_fullscreen_if_needed(window);
        conn_.flush();
    }
}

Geometry WindowManager::fullscreen_geometry_for_window(xcb_window_t window) const
{
    if (monitors_.empty())
        return {};

    auto it = fullscreen_monitors_.find(window);
    if (it == fullscreen_monitors_.end())
    {
        if (auto monitor_idx = monitor_index_for_window(window))
            return monitors_[*monitor_idx].geometry();
        return monitors_[0].geometry();
    }

    std::vector<size_t> indices;
    indices.reserve(4);
    auto const& spec = it->second;
    size_t total = monitors_.size();
    if (spec.top < total)
        indices.push_back(spec.top);
    if (spec.bottom < total)
        indices.push_back(spec.bottom);
    if (spec.left < total)
        indices.push_back(spec.left);
    if (spec.right < total)
        indices.push_back(spec.right);

    if (indices.empty())
    {
        if (auto monitor_idx = monitor_index_for_window(window))
            return monitors_[*monitor_idx].geometry();
        return monitors_[0].geometry();
    }

    int16_t min_x = monitors_[indices[0]].x;
    int16_t min_y = monitors_[indices[0]].y;
    int32_t max_x = monitors_[indices[0]].x + monitors_[indices[0]].width;
    int32_t max_y = monitors_[indices[0]].y + monitors_[indices[0]].height;

    for (size_t i = 1; i < indices.size(); ++i)
    {
        auto const& mon = monitors_[indices[i]];
        min_x = std::min<int16_t>(min_x, mon.x);
        min_y = std::min<int16_t>(min_y, mon.y);
        max_x = std::max<int32_t>(max_x, mon.x + mon.width);
        max_y = std::max<int32_t>(max_y, mon.y + mon.height);
    }

    Geometry area;
    area.x = min_x;
    area.y = min_y;
    area.width = static_cast<uint16_t>(std::max<int32_t>(1, max_x - min_x));
    area.height = static_cast<uint16_t>(std::max<int32_t>(1, max_y - min_y));
    return area;
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
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
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
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
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
    if (supports_protocol(window, wm_delete_window_))
    {
        xcb_client_message_event_t ev = {};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = window;
        ev.type = wm_protocols_;
        ev.format = 32;
        ev.data.data32[0] = wm_delete_window_;
        ev.data.data32[1] = last_event_time_ ? last_event_time_ : XCB_CURRENT_TIME;

        xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
        conn_.flush();

        send_wm_ping(window, last_event_time_);
        pending_kills_[window] = std::chrono::steady_clock::now() + KILL_TIMEOUT;
        return;
    }

    xcb_kill_client(conn_.get(), window);
    conn_.flush();
}

void WindowManager::rearrange_monitor(Monitor& monitor)
{
    if (showing_desktop_)
    {
        for (auto const& window : monitor.current().windows)
        {
            wm_unmap_window(window.id);
        }
        return;
    }

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

    monitor.previous_workspace = monitor.current_workspace;
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

void WindowManager::toggle_workspace()
{
    auto& monitor = focused_monitor();
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count <= 1)
        return;

    size_t target = monitor.previous_workspace;
    if (target >= workspace_count || target == monitor.current_workspace)
        return;

    switch_workspace(static_cast<int>(target));
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

    if (auto* floating_window = find_floating_window(window_to_move))
    {
        size_t target_ws = static_cast<size_t>(ws);
        size_t monitor_idx = floating_window->monitor;

        floating_window->workspace = target_ws;
        uint32_t desktop = get_ewmh_desktop_index(monitor_idx, target_ws);
        ewmh_.set_window_desktop(window_to_move, desktop);

        update_floating_visibility(monitor_idx);
        focus_or_fallback(monitors_[monitor_idx]);
        update_all_bars();
        conn_.flush();
        return;
    }

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

    auto eligible = [this](xcb_window_t window)
    { return window != XCB_NONE && !iconic_windows_.contains(window) && is_focus_eligible(window); };

    // Verify focused_window actually exists in the workspace (defensive programming)
    if (ws.focused_window != XCB_NONE && ws.find_window(ws.focused_window) != ws.windows.end()
        && eligible(ws.focused_window))
    {
        focus_window(ws.focused_window);
    }
    else
    {
        xcb_window_t candidate = XCB_NONE;
        for (auto it = ws.windows.rbegin(); it != ws.windows.rend(); ++it)
        {
            if (eligible(it->id))
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
                [&](FloatingWindow const& floating_window)
                {
                    return floating_window.monitor == monitor_idx
                        && floating_window.workspace == monitor.current_workspace && eligible(floating_window.id);
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

    if (active_window_ == XCB_NONE)
        return;

    xcb_window_t window_to_move = active_window_;

    if (auto* floating_window = find_floating_window(window_to_move))
    {
        size_t source_idx = floating_window->monitor;
        size_t target_idx = wrap_monitor_index(static_cast<int>(source_idx) + direction);
        if (target_idx == source_idx)
            return;

        floating_window->monitor = target_idx;
        floating_window->workspace = monitors_[target_idx].current_workspace;
        floating_window->geometry = floating::place_floating(
            monitors_[target_idx].working_area(),
            floating_window->geometry.width,
            floating_window->geometry.height,
            std::nullopt
        );

        uint32_t desktop = get_ewmh_desktop_index(target_idx, floating_window->workspace);
        ewmh_.set_window_desktop(window_to_move, desktop);

        update_floating_visibility(source_idx);
        update_floating_visibility(target_idx);

        focused_monitor_ = target_idx;
        update_ewmh_current_desktop();
        focus_floating_window(window_to_move);
        warp_to_monitor(monitors_[target_idx]);

        update_all_bars();
        conn_.flush();
        return;
    }

    auto& source_ws = focused_monitor().current();
    size_t target_idx = wrap_monitor_index(static_cast<int>(focused_monitor_) + direction);
    if (target_idx == focused_monitor_)
        return;

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

std::optional<uint32_t> WindowManager::get_window_desktop(xcb_window_t window) const
{
    uint32_t desktop = 0;
    if (!xcb_ewmh_get_wm_desktop_reply(
            ewmh_.get(),
            xcb_ewmh_get_wm_desktop(ewmh_.get(), window),
            &desktop,
            nullptr
        ))
    {
        return std::nullopt;
    }

    if (desktop == 0xFFFFFFFF)
        return std::nullopt;

    return desktop;
}

std::optional<std::pair<size_t, size_t>> WindowManager::resolve_window_desktop(xcb_window_t window) const
{
    if (config_.workspaces.count == 0)
        return std::nullopt;

    auto desktop = get_window_desktop(window);
    if (!desktop)
        return std::nullopt;

    size_t monitor_idx = static_cast<size_t>(*desktop / config_.workspaces.count);
    size_t workspace_idx = static_cast<size_t>(*desktop % config_.workspaces.count);

    if (monitor_idx >= monitors_.size())
        return std::nullopt;

    if (workspace_idx >= monitors_[monitor_idx].workspaces.size())
        return std::nullopt;

    return std::pair<size_t, size_t>{ monitor_idx, workspace_idx };
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

bool WindowManager::is_workspace_visible(size_t monitor_idx, size_t workspace_idx) const
{
    if (showing_desktop_)
        return false;
    if (monitor_idx >= monitors_.size())
        return false;
    return workspace_idx == monitors_[monitor_idx].current_workspace;
}

std::optional<WindowManager::ActiveWindowInfo> WindowManager::get_active_window_info() const
{
    if (active_window_ == XCB_NONE)
        return std::nullopt;

    if (auto const* floating_window = find_floating_window(active_window_))
    {
        ActiveWindowInfo info;
        info.monitor = floating_window->monitor;
        info.workspace = floating_window->workspace;
        info.title = floating_window->name.empty() ? "Unknown" : floating_window->name;
        return info;
    }

    for (size_t m = 0; m < monitors_.size(); ++m)
    {
        auto const& monitor = monitors_[m];
        for (size_t w = 0; w < monitor.workspaces.size(); ++w)
        {
            auto const& workspace = monitor.workspaces[w];
            auto it = workspace.find_window(active_window_);
            if (it != workspace.windows.end())
            {
                ActiveWindowInfo info;
                info.monitor = m;
                info.workspace = w;
                info.title = it->name.empty() ? "Unknown" : it->name;
                return info;
            }
        }
    }

    return std::nullopt;
}

BarState WindowManager::build_bar_state(size_t monitor_idx, std::optional<ActiveWindowInfo> const& active_info) const
{
    BarState state;
    if (monitor_idx >= monitors_.size())
        return state;

    auto const& monitor = monitors_[monitor_idx];
    state.workspace_has_windows.assign(monitor.workspaces.size(), 0);

    for (size_t i = 0; i < monitor.workspaces.size(); ++i)
    {
        if (!monitor.workspaces[i].windows.empty())
            state.workspace_has_windows[i] = 1;
    }

    for (auto const& floating_window : floating_windows_)
    {
        if (floating_window.monitor == monitor_idx && floating_window.workspace < state.workspace_has_windows.size())
        {
            state.workspace_has_windows[floating_window.workspace] = 1;
        }
    }

    if (active_info && active_info->monitor == monitor_idx && active_info->workspace == monitor.current_workspace)
    {
        state.focused_title = active_info->title;
        return state;
    }

    auto const& ws = monitor.current();
    if (ws.focused_window != XCB_NONE)
    {
        auto it = ws.find_window(ws.focused_window);
        if (it != ws.windows.end())
        {
            state.focused_title = it->name.empty() ? "Unknown" : it->name;
        }
        else
        {
            state.focused_title = "Unknown";
        }
    }

    return state;
}

void WindowManager::update_floating_visibility(size_t monitor_idx)
{
    if (monitor_idx >= monitors_.size())
        return;

    auto& monitor = monitors_[monitor_idx];
    if (showing_desktop_)
    {
        for (auto& floating_window : floating_windows_)
        {
            if (floating_window.monitor == monitor_idx)
            {
                wm_unmap_window(floating_window.id);
            }
        }
        return;
    }

    for (auto& floating_window : floating_windows_)
    {
        if (floating_window.monitor != monitor_idx)
            continue;

        if (floating_window.workspace == monitor.current_workspace && !iconic_windows_.contains(floating_window.id))
        {
            // Configure BEFORE mapping so window appears at correct position
            if (fullscreen_windows_.contains(floating_window.id))
            {
                apply_fullscreen_if_needed(floating_window.id);
            }
            else
            {
                apply_floating_geometry(floating_window);
            }
            xcb_map_window(conn_.get(), floating_window.id);
            wm_unmapped_windows_.erase(floating_window.id);
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
    auto new_monitor =
        focus::monitor_index_at_point(monitors_, static_cast<int16_t>(center_x), static_cast<int16_t>(center_y));
    if (!new_monitor || *new_monitor == window.monitor)
        return;

    window.monitor = *new_monitor;
    window.workspace = monitors_[window.monitor].current_workspace;

    uint32_t desktop = get_ewmh_desktop_index(window.monitor, window.workspace);
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

bool WindowManager::supports_protocol(xcb_window_t window, xcb_atom_t protocol) const
{
    if (protocol == XCB_NONE || wm_protocols_ == XCB_NONE)
        return false;

    xcb_icccm_get_wm_protocols_reply_t reply;
    if (!xcb_icccm_get_wm_protocols_reply(
            conn_.get(),
            xcb_icccm_get_wm_protocols(conn_.get(), window, wm_protocols_),
            &reply,
            nullptr
        ))
    {
        return false;
    }

    bool supported = false;
    for (uint32_t i = 0; i < reply.atoms_len; ++i)
    {
        if (reply.atoms[i] == protocol)
        {
            supported = true;
            break;
        }
    }
    xcb_icccm_get_wm_protocols_reply_wipe(&reply);
    return supported;
}

bool WindowManager::is_focus_eligible(xcb_window_t window) const
{
    xcb_icccm_wm_hints_t hints;
    if (!xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), window), &hints, nullptr))
        return true;

    if (!(hints.flags & XCB_ICCCM_WM_HINT_INPUT))
        return true;

    if (hints.input)
        return true;

    return supports_protocol(window, wm_take_focus_);
}

bool WindowManager::should_set_input_focus(xcb_window_t window) const
{
    xcb_icccm_wm_hints_t hints;
    if (!xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), window), &hints, nullptr))
        return true;

    if (!(hints.flags & XCB_ICCCM_WM_HINT_INPUT))
        return true;

    if (hints.input)
        return true;

    return false;
}

void WindowManager::send_wm_take_focus(xcb_window_t window, uint32_t timestamp)
{
    if (wm_protocols_ == XCB_NONE || wm_take_focus_ == XCB_NONE)
        return;

    if (!supports_protocol(window, wm_take_focus_))
        return;

    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = window;
    ev.type = wm_protocols_;
    ev.format = 32;
    ev.data.data32[0] = wm_take_focus_;
    ev.data.data32[1] = timestamp ? timestamp : XCB_CURRENT_TIME;

    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
}

void WindowManager::send_wm_ping(xcb_window_t window, uint32_t timestamp)
{
    if (wm_protocols_ == XCB_NONE || net_wm_ping_ == XCB_NONE)
        return;

    if (!supports_protocol(window, net_wm_ping_))
        return;

    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = window;
    ev.type = wm_protocols_;
    ev.format = 32;
    ev.data.data32[0] = net_wm_ping_;
    ev.data.data32[1] = timestamp ? timestamp : XCB_CURRENT_TIME;
    ev.data.data32[2] = window;

    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
    pending_pings_[window] = std::chrono::steady_clock::now() + PING_TIMEOUT;
}

void WindowManager::send_sync_request(xcb_window_t window, uint32_t timestamp)
{
    if (wm_protocols_ == XCB_NONE || net_wm_sync_request_ == XCB_NONE)
        return;

    auto it = sync_counters_.find(window);
    if (it == sync_counters_.end())
        return;

    uint64_t value = ++sync_values_[window];

    // _NET_WM_SYNC_REQUEST is sent via WM_PROTOCOLS (EWMH spec)
    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = window;
    ev.type = wm_protocols_;
    ev.format = 32;
    ev.data.data32[0] = net_wm_sync_request_;
    ev.data.data32[1] = timestamp ? timestamp : XCB_CURRENT_TIME;
    ev.data.data32[2] = static_cast<uint32_t>(value & 0xffffffff);
    ev.data.data32[3] = static_cast<uint32_t>((value >> 32) & 0xffffffff);

    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
    conn_.flush();
    wait_for_sync_counter(window, value);
}

bool WindowManager::wait_for_sync_counter(xcb_window_t window, uint64_t expected_value)
{
    auto it = sync_counters_.find(window);
    if (it == sync_counters_.end())
        return false;

    xcb_sync_counter_t counter = it->second;
    auto deadline = std::chrono::steady_clock::now() + SYNC_WAIT_TIMEOUT;

    while (std::chrono::steady_clock::now() < deadline)
    {
        auto cookie = xcb_sync_query_counter(conn_.get(), counter);
        auto* reply = xcb_sync_query_counter_reply(conn_.get(), cookie, nullptr);
        if (!reply)
            return false;

        uint64_t current = (static_cast<uint64_t>(reply->counter_value.hi) << 32)
            | static_cast<uint64_t>(reply->counter_value.lo);
        free(reply);

        if (current >= expected_value)
        {
            sync_values_[window] = current;
            return true;
        }

        usleep(1000);
    }

    return false;
}

void WindowManager::update_sync_state(xcb_window_t window)
{
    if (net_wm_sync_request_counter_ == XCB_NONE || net_wm_sync_request_ == XCB_NONE)
        return;

    if (!supports_protocol(window, net_wm_sync_request_))
    {
        sync_counters_.erase(window);
        sync_values_.erase(window);
        return;
    }

    auto cookie = xcb_get_property(conn_.get(), 0, window, net_wm_sync_request_counter_, XCB_ATOM_CARDINAL, 0, 1);
    auto* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);
    if (!reply)
        return;

    xcb_sync_counter_t counter = XCB_NONE;
    if (xcb_get_property_value_length(reply) >= static_cast<int>(sizeof(xcb_sync_counter_t)))
    {
        auto* value = static_cast<xcb_sync_counter_t*>(xcb_get_property_value(reply));
        if (value)
            counter = *value;
    }
    free(reply);

    if (counter == XCB_NONE)
    {
        sync_counters_.erase(window);
        sync_values_.erase(window);
        return;
    }

    sync_counters_[window] = counter;

    auto counter_cookie = xcb_sync_query_counter(conn_.get(), counter);
    auto* counter_reply = xcb_sync_query_counter_reply(conn_.get(), counter_cookie, nullptr);
    if (counter_reply)
    {
        uint64_t value = (static_cast<uint64_t>(counter_reply->counter_value.hi) << 32)
            | static_cast<uint64_t>(counter_reply->counter_value.lo);
        sync_values_[window] = value;
        free(counter_reply);
    }
    else
    {
        sync_values_[window] = 0;
    }
}

void WindowManager::update_fullscreen_monitor_state(xcb_window_t window)
{
    if (net_wm_fullscreen_monitors_ == XCB_NONE)
        return;

    xcb_ewmh_get_wm_fullscreen_monitors_reply_t reply;
    if (!xcb_ewmh_get_wm_fullscreen_monitors_reply(
            ewmh_.get(),
            xcb_ewmh_get_wm_fullscreen_monitors(ewmh_.get(), window),
            &reply,
            nullptr
        ))
    {
        fullscreen_monitors_.erase(window);
        return;
    }

    FullscreenMonitors monitors;
    monitors.top = reply.top;
    monitors.bottom = reply.bottom;
    monitors.left = reply.left;
    monitors.right = reply.right;
    fullscreen_monitors_[window] = monitors;
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

    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<char*>(&ev));

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
    drag_state_.tiled = false;
    drag_state_.resizing = resize;
    drag_state_.window = window;
    drag_state_.start_root_x = root_x;
    drag_state_.start_root_y = root_y;
    drag_state_.last_root_x = root_x;
    drag_state_.last_root_y = root_y;
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

void WindowManager::begin_tiled_drag(xcb_window_t window, int16_t root_x, int16_t root_y)
{
    if (showing_desktop_)
        return;
    if (fullscreen_windows_.contains(window))
        return;
    if (find_floating_window(window))
        return;
    if (!monitor_containing_window(window))
        return;

    auto geom_cookie = xcb_get_geometry(conn_.get(), window);
    auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
    if (!geom_reply)
        return;

    drag_state_.active = true;
    drag_state_.tiled = true;
    drag_state_.resizing = false;
    drag_state_.window = window;
    drag_state_.start_root_x = root_x;
    drag_state_.start_root_y = root_y;
    drag_state_.last_root_x = root_x;
    drag_state_.last_root_y = root_y;
    drag_state_.start_geometry = { geom_reply->x, geom_reply->y, geom_reply->width, geom_reply->height };
    free(geom_reply);

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

    drag_state_.last_root_x = root_x;
    drag_state_.last_root_y = root_y;

    if (drag_state_.tiled)
    {
        int32_t dx = static_cast<int32_t>(root_x) - static_cast<int32_t>(drag_state_.start_root_x);
        int32_t dy = static_cast<int32_t>(root_y) - static_cast<int32_t>(drag_state_.start_root_y);
        int32_t new_x = static_cast<int32_t>(drag_state_.start_geometry.x) + dx;
        int32_t new_y = static_cast<int32_t>(drag_state_.start_geometry.y) + dy;
        uint32_t values[] = { static_cast<uint32_t>(new_x), static_cast<uint32_t>(new_y) };
        xcb_configure_window(conn_.get(), drag_state_.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
        conn_.flush();
        return;
    }

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

        uint32_t hinted_width = updated.width;
        uint32_t hinted_height = updated.height;
        layout_.apply_size_hints(floating_window->id, hinted_width, hinted_height);
        updated.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
        updated.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));
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

    if (drag_state_.tiled)
    {
        xcb_window_t window = drag_state_.window;
        auto source_monitor_idx = monitor_index_for_window(window);
        auto source_workspace_idx = workspace_index_for_window(window);
        if (source_monitor_idx && source_workspace_idx && !monitors_.empty())
        {
            auto target_monitor =
                focus::monitor_index_at_point(monitors_, drag_state_.last_root_x, drag_state_.last_root_y);
            size_t target_monitor_idx = target_monitor.value_or(*source_monitor_idx);
            if (target_monitor_idx < monitors_.size())
            {
                size_t target_workspace_idx = monitors_[target_monitor_idx].current_workspace;
                bool same_workspace = *source_monitor_idx == target_monitor_idx
                    && *source_workspace_idx == target_workspace_idx;

                auto& source_ws = monitors_[*source_monitor_idx].workspaces[*source_workspace_idx];
                auto source_it = source_ws.find_window(window);
                if (source_it != source_ws.windows.end())
                {
                    Window moved_window = *source_it;

                    auto& target_ws = monitors_[target_monitor_idx].workspaces[target_workspace_idx];
                    size_t layout_count = target_ws.windows.size();
                    if (!same_workspace)
                        layout_count += 1;

                    source_ws.windows.erase(source_it);

                    size_t target_index = 0;
                    if (layout_count > 0)
                    {
                        target_index = layout_.drop_target_index(
                            layout_count,
                            monitors_[target_monitor_idx].working_area(),
                            bar_.has_value(),
                            drag_state_.last_root_x,
                            drag_state_.last_root_y
                        );
                    }

                    size_t insert_index = std::min(target_index, target_ws.windows.size());
                    target_ws.windows.insert(target_ws.windows.begin() + insert_index, moved_window);
                    target_ws.focused_window = window;

                    if (!same_workspace && source_ws.focused_window == window)
                    {
                        source_ws.focused_window =
                            source_ws.windows.empty() ? XCB_NONE : source_ws.windows.back().id;
                    }

                    if (!same_workspace)
                    {
                        uint32_t desktop = get_ewmh_desktop_index(target_monitor_idx, target_workspace_idx);
                        ewmh_.set_window_desktop(window, desktop);
                    }

                    if (*source_monitor_idx == target_monitor_idx)
                    {
                        rearrange_monitor(monitors_[target_monitor_idx]);
                    }
                    else
                    {
                        rearrange_monitor(monitors_[*source_monitor_idx]);
                        rearrange_monitor(monitors_[target_monitor_idx]);
                    }

                    update_ewmh_client_list();
                    update_all_bars();
                    focus_window(window);
                }
            }
        }
    }

    drag_state_.active = false;
    drag_state_.tiled = false;
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
        auto cookie = xcb_get_property(conn_.get(), 0, window, ewmh_.get()->_NET_WM_NAME, utf8_string_, 0, 1024);
        auto* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);
        if (reply)
        {
            int len = xcb_get_property_value_length(reply);
            if (len > 0)
            {
                char* name = static_cast<char*>(xcb_get_property_value(reply));
                if (name)
                {
                    std::string windowName(name, len);
                    free(reply);
                    return windowName;
                }
            }
            free(reply);
        }
    }

    auto cookie = xcb_get_property(conn_.get(), 0, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 1024);
    auto* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);

    if (reply)
    {
        int len = xcb_get_property_value_length(reply);
        if (len > 0)
        {
            char* name = static_cast<char*>(xcb_get_property_value(reply));
            if (name)
            {
                std::string windowName(name, len);
                free(reply);
                return windowName;
            }
        }
        free(reply);
    }
    return "Unnamed";
}

std::pair<std::string, std::string> WindowManager::get_wm_class(xcb_window_t window)
{
    xcb_icccm_get_wm_class_reply_t wm_class;
    if (xcb_icccm_get_wm_class_reply(conn_.get(), xcb_icccm_get_wm_class(conn_.get(), window), &wm_class, nullptr))
    {
        std::string class_name = wm_class.class_name ? wm_class.class_name : "";
        std::string instance_name = wm_class.instance_name ? wm_class.instance_name : "";
        xcb_icccm_get_wm_class_reply_wipe(&wm_class);
        return { instance_name, class_name };
    }
    return { "", "" };
}

uint32_t WindowManager::get_user_time(xcb_window_t window)
{
    if (net_wm_user_time_ == XCB_NONE)
        return 0;

    auto cookie = xcb_get_property(conn_.get(), 0, window, net_wm_user_time_, XCB_ATOM_CARDINAL, 0, 1);
    auto* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);
    if (reply)
    {
        uint32_t time = 0;
        if (xcb_get_property_value_length(reply) >= 4)
        {
            time = *static_cast<uint32_t*>(xcb_get_property_value(reply));
        }
        free(reply);
        return time;
    }
    return 0;
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

    if (auto* floating_window = find_floating_window(window))
    {
        if (floating_window->name != name)
        {
            floating_window->name = name;
            if (is_workspace_visible(floating_window->monitor, floating_window->workspace))
                update_bars = true;
        }
    }

    if (active_window_ == window)
        update_bars = true;

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

    auto active_info = get_active_window_info();
    for (size_t i = 0; i < monitors_.size(); ++i)
    {
        bar_->update(monitors_[i], build_bar_state(i, active_info));
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

    update_ewmh_workarea();
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

    // Calculate total desktop geometry (bounding box of all monitors)
    uint32_t max_x = 0;
    uint32_t max_y = 0;
    for (auto const& monitor : monitors_)
    {
        uint32_t right = static_cast<uint32_t>(monitor.x) + monitor.width;
        uint32_t bottom = static_cast<uint32_t>(monitor.y) + monitor.height;
        max_x = std::max(max_x, right);
        max_y = std::max(max_y, bottom);
    }
    ewmh_.set_desktop_geometry(max_x, max_y);

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
    update_ewmh_workarea();
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

    // Build stacking order: below < tiled < floating < above < fullscreen
    std::vector<xcb_window_t> stacking;
    stacking.reserve(windows.size());

    auto is_below = [this](xcb_window_t id)
    {
        return below_windows_.contains(id) && !fullscreen_windows_.contains(id);
    };

    auto is_above = [this](xcb_window_t id)
    {
        return above_windows_.contains(id) && !fullscreen_windows_.contains(id);
    };

    // Helper to check if window is in "normal" tier (not below, not above, not fullscreen)
    auto is_normal = [this](xcb_window_t id)
    {
        return !below_windows_.contains(id) && !above_windows_.contains(id) && !fullscreen_windows_.contains(id);
    };

    auto append_tiled = [&](auto predicate)
    {
        for (auto const& monitor : monitors_)
        {
            for (auto const& workspace : monitor.workspaces)
            {
                for (auto const& window : workspace.windows)
                {
                    if (predicate(window.id))
                        stacking.push_back(window.id);
                }
            }
        }
    };

    auto append_floating = [&](auto predicate)
    {
        for (auto const& floating_window : floating_windows_)
        {
            if (predicate(floating_window.id))
                stacking.push_back(floating_window.id);
        }
    };

    // BELOW windows (bottom of stack)
    append_tiled(is_below);
    append_floating(is_below);

    // Normal windows
    append_tiled(is_normal);
    append_floating(is_normal);

    // ABOVE windows
    append_tiled(is_above);
    append_floating(is_above);

    // Fullscreen windows (top of stack, deterministic order)
    append_tiled([this](xcb_window_t id) { return fullscreen_windows_.contains(id); });
    append_floating([this](xcb_window_t id) { return fullscreen_windows_.contains(id); });

    ewmh_.update_client_list_stacking(stacking);
}

void WindowManager::update_ewmh_current_desktop()
{
    uint32_t desktop = get_ewmh_desktop_index(focused_monitor_, focused_monitor().current_workspace);
    ewmh_.set_current_desktop(desktop);
}

void WindowManager::update_ewmh_workarea()
{
    std::vector<Geometry> workareas;
    workareas.reserve(monitors_.size() * config_.workspaces.count);
    for (auto const& monitor : monitors_)
    {
        Geometry area = monitor.working_area();
        if (bar_)
        {
            uint32_t bar_height = config_.appearance.status_bar_height;
            if (area.height > bar_height)
            {
                area.y = static_cast<int16_t>(area.y + bar_height);
                area.height = static_cast<uint16_t>(area.height - bar_height);
            }
            else
            {
                area.height = 1;
            }
        }
        for (size_t i = 0; i < config_.workspaces.count; ++i)
        {
            workareas.push_back(area);
        }
    }
    ewmh_.set_workarea(workareas);
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

    size_t old_workspace = monitor.current_workspace;
    // Unmap windows from OLD workspace before switching
    for (auto const& window : monitor.current().windows)
    {
        wm_unmap_window(window.id);
    }

    // Switch to target monitor and workspace
    focused_monitor_ = monitor_idx;
    if (workspace_idx != old_workspace)
    {
        monitor.previous_workspace = old_workspace;
    }
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
                xcb_change_window_attributes(conn_.get(), window.id, XCB_CW_BORDER_PIXEL, &conn_.screen()->black_pixel);
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
