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
// Sync wait timeout is intentionally short to minimize blocking during layout.
// A fully non-blocking async implementation would be ideal but requires significant
// architectural changes. Most clients respond quickly; this timeout is a compromise.
constexpr auto SYNC_WAIT_TIMEOUT = std::chrono::milliseconds(50);

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
    net_wm_user_time_window_ = intern_atom("_NET_WM_USER_TIME_WINDOW");
    net_wm_state_focused_ = intern_atom("_NET_WM_STATE_FOCUSED");
    // Add extra supported atoms not in xcb_ewmh library
    {
        std::vector<xcb_atom_t> extra;
        if (net_wm_user_time_window_ != XCB_NONE)
            extra.push_back(net_wm_user_time_window_);
        if (net_wm_state_focused_ != XCB_NONE)
            extra.push_back(net_wm_state_focused_);
        if (!extra.empty())
            ewmh_.set_extra_supported_atoms(extra);
    }
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

        // Use centralized classification
        bool has_transient = transient_for_window(window).has_value();
        auto classification = ewmh_.classify_window(window, has_transient);

        switch (classification.kind)
        {
            case WindowClassification::Kind::Desktop:
            {
                uint32_t values[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
                xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
                uint32_t stack_mode = XCB_STACK_MODE_BELOW;
                xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
                if (std::ranges::find(desktop_windows_, window) == desktop_windows_.end())
                {
                    desktop_windows_.push_back(window);
                    Client client;
                    client.id = window;
                    client.kind = Client::Kind::Desktop;
                    client.skip_taskbar = true;
                    client.skip_pager = true;
                    client.order = next_client_order_++;
                    clients_[window] = std::move(client);
                }
                break;
            }

            case WindowClassification::Kind::Dock:
            {
                uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_POINTER_MOTION
                                      | XCB_EVENT_MASK_PROPERTY_CHANGE };
                xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
                if (std::ranges::find(dock_windows_, window) == dock_windows_.end())
                {
                    dock_windows_.push_back(window);
                    Client client;
                    client.id = window;
                    client.kind = Client::Kind::Dock;
                    client.skip_taskbar = true;
                    client.skip_pager = true;
                    client.order = next_client_order_++;
                    clients_[window] = std::move(client);
                    update_struts();
                }
                break;
            }

            case WindowClassification::Kind::Popup:
                // Popup windows (already mapped) are not managed
                break;

            case WindowClassification::Kind::Floating:
            {
                manage_floating_window(window);
                if (classification.skip_taskbar)
                    set_client_skip_taskbar(window, true);
                if (classification.skip_pager)
                    set_client_skip_pager(window, true);
                if (classification.above)
                    set_window_above(window, true);
                break;
            }

            case WindowClassification::Kind::Tiled:
            {
                manage_window(window);
                break;
            }
        }
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

// Event handlers are implemented in wm_events.cpp
// Drag operations are implemented in wm_drag.cpp

// NOTE: All handle_* functions have been moved to wm_events.cpp
// NOTE: begin_drag, begin_tiled_drag, update_drag, end_drag have been moved to wm_drag.cpp

// ─────────────────────────────────────────────────────────────────────────────
// Window Management
// ─────────────────────────────────────────────────────────────────────────────


void WindowManager::manage_window(xcb_window_t window, bool start_iconic)
{
    auto [instance_name, class_name] = get_wm_class(window);
    Window newWindow = { window, get_window_name(window), class_name, instance_name };
    auto target = resolve_window_desktop(window);
    size_t target_monitor_idx = target ? target->first : focused_monitor_;
    size_t target_workspace_idx =
        target ? target->second : monitors_[target_monitor_idx].current_workspace;

    monitors_[target_monitor_idx].workspaces[target_workspace_idx].windows.push_back(newWindow);

    // Populate unified Client registry
    {
        Client client;
        client.id = window;
        client.kind = Client::Kind::Tiled;
        client.monitor = target_monitor_idx;
        client.workspace = target_workspace_idx;
        client.name = newWindow.name;
        client.wm_class = newWindow.wm_class;
        client.wm_class_name = newWindow.wm_class_name;
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

        // Read transient_for relationship
        client.transient_for = transient_for_window(window).value_or(XCB_NONE);

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
                    user_time_windows_[window] = time_window;
                    // Also update Client (Phase 3)
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
    user_times_[window] = get_user_time(window);
    // Also update Client (Phase 3)
    if (auto* client = get_client(window))
        client->user_time = user_times_[window];

    // Note: We intentionally do NOT select STRUCTURE_NOTIFY on client windows.
    // We receive UnmapNotify/DestroyNotify via root's SubstructureNotifyMask.
    // Selecting STRUCTURE_NOTIFY on clients would cause duplicate UnmapNotify events,
    // leading to incorrect unmanagement of windows during workspace switches (ICCCM compliance).
    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE
                          | XCB_EVENT_MASK_PROPERTY_CHANGE };
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
        // client->iconic is already set in the Client initialization above
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
        ewmh->_NET_WM_ACTION_SHADE, ewmh->_NET_WM_ACTION_STICK, ewmh->_NET_WM_ACTION_MAXIMIZE_VERT,
        ewmh->_NET_WM_ACTION_MAXIMIZE_HORZ,
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

    // Honor existing _NET_WM_STATE flags
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
    floating_window.transient_for = transient.value_or(XCB_NONE);
    floating_windows_.push_back(floating_window);

    // Populate unified Client registry
    {
        auto [instance_name, class_name] = get_wm_class(window);
        Client client;
        client.id = window;
        client.kind = Client::Kind::Floating;
        client.monitor = *monitor_idx;
        client.workspace = *workspace_idx;
        client.name = floating_window.name;
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
                    user_time_windows_[window] = time_window;
                    // Also update Client (Phase 3)
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
    user_times_[window] = get_user_time(window);
    // Also update Client (Phase 3)
    if (auto* client = get_client(window))
        client->user_time = user_times_[window];

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
    if (!start_iconic)
    {
        update_floating_visibility(*monitor_idx);
        if (!suppress_focus_ && *monitor_idx == focused_monitor_ && is_workspace_visible(*monitor_idx, *workspace_idx))
            focus_floating_window(window);
    }

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

    // Tracking state that remains outside Client
    fullscreen_monitors_.erase(window);
    sync_counters_.erase(window);
    sync_values_.erase(window);
    pending_kills_.erase(window);
    pending_pings_.erase(window);
    user_times_.erase(window);
    user_time_windows_.erase(window);

    // Remove from unified Client registry (handles all client state including order)
    clients_.erase(window);

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

    // Tracking state that remains outside Client
    fullscreen_monitors_.erase(window);
    sync_counters_.erase(window);
    sync_values_.erase(window);
    pending_kills_.erase(window);
    pending_pings_.erase(window);
    user_times_.erase(window);
    user_time_windows_.erase(window);

    // Remove from unified Client registry (handles all client state including order)
    clients_.erase(window);

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

    if (is_client_iconic(window))
    {
        deiconify_window(window, false);
    }

    xcb_window_t previous_active = active_window_;

    // Sticky windows are visible on all workspaces - focusing them should NOT switch workspaces.
    bool is_sticky = is_client_sticky(window);
    auto change = focus::focus_window_state(monitors_, focused_monitor_, active_window_, window, is_sticky);
    if (!change)
        return;

    auto& target_monitor = monitors_[change->target_monitor];
    if (change->workspace_changed)
    {
        for (auto const& w : target_monitor.workspaces[change->old_workspace].windows)
        {
            if (is_client_sticky(w.id))
                continue;
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
    // Use event timestamp for SetInputFocus to ensure proper focus ordering.
    // Using CurrentTime can cause focus to be ignored or reordered incorrectly.
    xcb_timestamp_t focus_time = last_event_time_ ? last_event_time_ : XCB_CURRENT_TIME;
    send_wm_take_focus(window, last_event_time_);
    if (should_set_input_focus(window))
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, window, focus_time);
    }
    else
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, focus_time);
    }

    // Clear urgent hint when window receives focus
    set_client_demands_attention(window, false);
    ewmh_.set_active_window(window);
    if (net_wm_state_focused_ != XCB_NONE)
    {
        if (previous_active != XCB_NONE && previous_active != window)
        {
            ewmh_.set_window_state(previous_active, net_wm_state_focused_, false);
        }
        ewmh_.set_window_state(window, net_wm_state_focused_, true);
    }
    user_times_[window] = last_event_time_;
    // Keep Client.user_time in sync
    if (auto* client = get_client(window))
        client->user_time = last_event_time_;

    apply_fullscreen_if_needed(window);
    restack_transients(window);
    // Update stacking order in EWMH after restacking transients
    update_ewmh_client_list();

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

    if (is_client_iconic(window))
    {
        deiconify_window(window, false);
    }

    if (floating_window->monitor >= monitors_.size())
        return;

    xcb_window_t previous_active = active_window_;

    // Sticky windows are visible on all workspaces - focusing them should NOT switch workspaces.
    bool is_sticky = is_client_sticky(window);

    focused_monitor_ = floating_window->monitor;
    auto& monitor = monitors_[floating_window->monitor];
    if (!is_sticky && monitor.current_workspace != floating_window->workspace)
    {
        for (auto const& w : monitor.current().windows)
        {
            if (is_client_sticky(w.id))
                continue;
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
    // Use event timestamp for SetInputFocus to ensure proper focus ordering.
    xcb_timestamp_t focus_time = last_event_time_ ? last_event_time_ : XCB_CURRENT_TIME;
    send_wm_take_focus(window, last_event_time_);
    if (should_set_input_focus(window))
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, window, focus_time);
    }
    else
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, focus_time);
    }

    uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);

    set_client_demands_attention(window, false);
    ewmh_.set_active_window(window);
    if (net_wm_state_focused_ != XCB_NONE)
    {
        if (previous_active != XCB_NONE && previous_active != window)
        {
            ewmh_.set_window_state(previous_active, net_wm_state_focused_, false);
        }
        ewmh_.set_window_state(window, net_wm_state_focused_, true);
    }
    user_times_[window] = last_event_time_;
    // Keep Client.user_time in sync
    if (auto* client = get_client(window))
        client->user_time = last_event_time_;

    apply_fullscreen_if_needed(window);
    restack_transients(window);

    update_ewmh_client_list();
    update_all_bars();
    conn_.flush();
}

void WindowManager::set_fullscreen(xcb_window_t window, bool enabled)
{
    auto monitor_idx = monitor_index_for_window(window);
    if (!monitor_idx)
        return;

    auto* client = get_client(window);
    if (!client)
        return;  // Only managed clients can be fullscreen

    if (enabled)
    {
        if (!client->fullscreen)
        {
            // Save restore geometry before going fullscreen
            if (auto* floating_window = find_floating_window(window))
            {
                client->fullscreen_restore = floating_window->geometry;
            }
            else
            {
                auto geom_cookie = xcb_get_geometry(conn_.get(), window);
                auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
                if (geom_reply)
                {
                    client->fullscreen_restore =
                        Geometry{ geom_reply->x, geom_reply->y, geom_reply->width, geom_reply->height };
                    free(geom_reply);
                }
            }
        }

        client->fullscreen = true;

        // Fullscreen is incompatible with ABOVE/BELOW states - clear them
        if (client->above)
        {
            client->above = false;
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE, false);
        }
        if (client->below)
        {
            client->below = false;
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_BELOW, false);
        }

        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN, true);
        apply_fullscreen_if_needed(window);
    }
    else
    {
        if (!client->fullscreen)
            return;

        client->fullscreen = false;
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN, false);

        if (auto* floating_window = find_floating_window(window))
        {
            if (client->fullscreen_restore)
            {
                floating_window->geometry = *client->fullscreen_restore;
                if (floating_window->workspace == monitors_[floating_window->monitor].current_workspace
                    && !client->iconic)
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

        client->fullscreen_restore = std::nullopt;
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

    auto* client = get_client(window);
    if (!client)
        return;  // Only managed clients can have above state

    if (enabled)
    {
        client->above = true;
        if (client->below)
        {
            client->below = false;
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_BELOW, false);
        }
        uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
    }
    else
    {
        client->above = false;
    }

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE, enabled);
    update_ewmh_client_list();
    conn_.flush();
}

void WindowManager::set_window_below(xcb_window_t window, bool enabled)
{
    if (!monitor_index_for_window(window))
        return;

    auto* client = get_client(window);
    if (!client)
        return;  // Only managed clients can have below state

    if (enabled)
    {
        client->below = true;
        if (client->above)
        {
            client->above = false;
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE, false);
        }
        uint32_t stack_mode = XCB_STACK_MODE_BELOW;
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
    }
    else
    {
        client->below = false;
    }

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_BELOW, enabled);
    update_ewmh_client_list();
    conn_.flush();
}

void WindowManager::set_window_sticky(xcb_window_t window, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return;  // Only managed clients can be sticky

    if (enabled)
    {
        client->sticky = true;
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_STICKY, true);
        ewmh_.set_window_desktop(window, 0xFFFFFFFF);
    }
    else
    {
        client->sticky = false;
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_STICKY, false);
        if (auto monitor_idx = monitor_index_for_window(window))
        {
            size_t workspace_idx = workspace_index_for_window(window).value_or(monitors_[*monitor_idx].current_workspace);
            uint32_t desktop = get_ewmh_desktop_index(*monitor_idx, workspace_idx);
            ewmh_.set_window_desktop(window, desktop);
        }
    }

    if (auto* floating_window = find_floating_window(window))
    {
        update_floating_visibility(floating_window->monitor);
    }
    else if (auto* monitor = monitor_containing_window(window))
    {
        rearrange_monitor(*monitor);
    }

    update_ewmh_client_list();
    update_all_bars();
    conn_.flush();
}

void WindowManager::set_window_maximized(xcb_window_t window, bool horiz, bool vert)
{
    auto* client = get_client(window);
    if (!client)
        return;  // Only managed clients can be maximized

    client->maximized_horz = horiz;
    client->maximized_vert = vert;

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_HORZ, horiz);
    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_VERT, vert);

    if (!client->fullscreen)
    {
        if (!horiz && !vert)
        {
            if (client->maximize_restore)
            {
                if (auto* floating_window = find_floating_window(window))
                {
                    floating_window->geometry = *client->maximize_restore;
                    if (floating_window->workspace == monitors_[floating_window->monitor].current_workspace
                        && !client->iconic)
                    {
                        apply_floating_geometry(*floating_window);
                    }
                }
                client->maximize_restore = std::nullopt;
            }
        }
        else if (auto* floating_window = find_floating_window(window))
        {
            if (!client->maximize_restore)
            {
                client->maximize_restore = floating_window->geometry;
            }
            apply_maximized_geometry(window);
        }
    }

    update_ewmh_client_list();
    update_all_bars();
    conn_.flush();
}

void WindowManager::apply_maximized_geometry(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;

    auto* floating_window = find_floating_window(window);
    if (!floating_window)
        return;
    if (floating_window->monitor >= monitors_.size())
        return;

    Geometry base = floating_window->geometry;
    if (client->maximize_restore)
    {
        base = *client->maximize_restore;
    }

    Geometry area = monitors_[floating_window->monitor].working_area();
    if (client->maximized_horz)
    {
        base.x = area.x;
        base.width = area.width;
    }
    if (client->maximized_vert)
    {
        base.y = area.y;
        base.height = area.height;
    }

    floating_window->geometry = base;
    if (floating_window->workspace == monitors_[floating_window->monitor].current_workspace
        && !client->iconic)
    {
        apply_floating_geometry(*floating_window);
    }
}

void WindowManager::set_window_shaded(xcb_window_t window, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return;  // Only managed clients can be shaded

    if (enabled)
    {
        if (!client->shaded)
        {
            client->shaded = true;
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_SHADED, true);
            if (!client->iconic)
            {
                iconify_window(window);
            }
        }
    }
    else
    {
        if (client->shaded)
        {
            client->shaded = false;
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_SHADED, false);
            if (client->iconic)
            {
                deiconify_window(window, false);
            }
        }
    }
}

void WindowManager::set_window_modal(xcb_window_t window, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return;  // Only managed clients can be modal

    client->modal = enabled;

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MODAL, enabled);
    if (enabled)
    {
        set_window_above(window, true);
    }
}

void WindowManager::set_client_skip_taskbar(xcb_window_t window, bool enabled)
{
    if (auto* client = get_client(window))
        client->skip_taskbar = enabled;

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_TASKBAR, enabled);
}

void WindowManager::set_client_skip_pager(xcb_window_t window, bool enabled)
{
    if (auto* client = get_client(window))
        client->skip_pager = enabled;

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_PAGER, enabled);
}

void WindowManager::set_client_demands_attention(xcb_window_t window, bool enabled)
{
    if (auto* client = get_client(window))
        client->demands_attention = enabled;

    ewmh_.set_demands_attention(window, enabled);
}

void WindowManager::apply_fullscreen_if_needed(xcb_window_t window)
{
    if (!is_client_fullscreen(window))
        return;
    if (is_client_iconic(window))
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

    if (is_client_fullscreen(window))
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

    auto* client = get_client(window);
    if (!client)
        return;  // Only managed clients can be iconified

    if (client->iconic)
        return;

    client->iconic = true;

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

    auto* client = get_client(window);
    if (!client)
        return;  // Only managed clients can be deiconified

    client->iconic = false;

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
    if (active_window_ != XCB_NONE && net_wm_state_focused_ != XCB_NONE)
    {
        ewmh_.set_window_state(active_window_, net_wm_state_focused_, false);
    }
    active_window_ = XCB_NONE;
    // Use event timestamp for SetInputFocus when available
    xcb_timestamp_t focus_time = last_event_time_ ? last_event_time_ : XCB_CURRENT_TIME;
    xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, focus_time);
    ewmh_.set_active_window(XCB_NONE);
}

/**
 * @brief Initiate window close with graceful fallback to force kill.
 *
 * Protocol:
 * 1. If window supports WM_DELETE_WINDOW, send the close request.
 * 2. Send _NET_WM_PING to check if the window is responsive.
 * 3. If ping response is received within timeout, the window is responsive
 *    and will close itself. Cancel the pending force kill.
 * 4. If ping times out without response, the window is hung - force kill it.
 *
 * If window doesn't support WM_DELETE_WINDOW, kill it immediately.
 */
void WindowManager::kill_window(xcb_window_t window)
{
    if (supports_protocol(window, wm_delete_window_))
    {
        // Send WM_DELETE_WINDOW request
        xcb_client_message_event_t ev = {};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = window;
        ev.type = wm_protocols_;
        ev.format = 32;
        ev.data.data32[0] = wm_delete_window_;
        ev.data.data32[1] = last_event_time_ ? last_event_time_ : XCB_CURRENT_TIME;

        xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
        conn_.flush();

        // Send ping to check if window is responsive.
        // If ping response arrives, pending_kills_ entry is removed (window is responsive).
        // If ping times out, force kill the hung window.
        send_wm_ping(window, last_event_time_);
        pending_kills_[window] = std::chrono::steady_clock::now() + KILL_TIMEOUT;
        return;
    }

    // Window doesn't support graceful close - force kill immediately
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
    std::unordered_set<xcb_window_t> seen;
    for (auto const& window : monitor.current().windows)
    {
        if (is_client_iconic(window.id))
        {
            wm_unmap_window(window.id);
            continue;
        }
        visible_windows.push_back(window);
        seen.insert(window.id);
    }

    for (auto const& workspace : monitor.workspaces)
    {
        for (auto const& window : workspace.windows)
        {
            if (!is_client_sticky(window.id))
                continue;
            if (is_client_iconic(window.id))
            {
                wm_unmap_window(window.id);
                continue;
            }
            if (!seen.contains(window.id))
            {
                visible_windows.push_back(window);
                seen.insert(window.id);
            }
        }
    }

    layout_.arrange(visible_windows, monitor.working_area(), bar_.has_value());

    for (auto const& window : visible_windows)
    {
        apply_fullscreen_if_needed(window.id);
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
        if (is_client_sticky(window.id))
            continue;
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
        // Update Client workspace for O(1) lookup
        if (auto* client = get_client(window_to_move))
            client->workspace = target_ws;

        uint32_t desktop = get_ewmh_desktop_index(monitor_idx, target_ws);
        if (is_client_sticky(window_to_move))
            ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
        else
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
    if (current_ws.focused_window == window_to_move)
    {
        current_ws.focused_window = XCB_NONE;
        for (auto rit = current_ws.windows.rbegin(); rit != current_ws.windows.rend(); ++rit)
        {
            if (!is_client_iconic(rit->id))
            {
                current_ws.focused_window = rit->id;
                break;
            }
        }
    }

    size_t target_ws = static_cast<size_t>(ws);
    monitor.workspaces[target_ws].windows.push_back(moved_window);
    monitor.workspaces[target_ws].focused_window = window_to_move;

    // Update Client workspace for O(1) lookup
    if (auto* client = get_client(window_to_move))
        client->workspace = target_ws;

    // Update EWMH desktop for moved window
    uint32_t desktop = get_ewmh_desktop_index(focused_monitor_, target_ws);
    if (is_client_sticky(window_to_move))
        ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
    else
        ewmh_.set_window_desktop(window_to_move, desktop);

    if (!is_client_sticky(window_to_move))
    {
        wm_unmap_window(window_to_move);
    }
    rearrange_monitor(monitor);
    focus_or_fallback(monitor);

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
    { return window != XCB_NONE && !is_client_iconic(window) && is_focus_eligible(window); };

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
    if (config_.focus.warp_cursor_on_monitor_change)
    {
        warp_to_monitor(monitor);
    }

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

        // Update Client for O(1) lookup
        if (auto* client = get_client(window_to_move))
        {
            client->monitor = target_idx;
            client->workspace = floating_window->workspace;
        }

        uint32_t desktop = get_ewmh_desktop_index(target_idx, floating_window->workspace);
        if (is_client_sticky(window_to_move))
            ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
        else
            ewmh_.set_window_desktop(window_to_move, desktop);

        update_floating_visibility(source_idx);
        update_floating_visibility(target_idx);

        focused_monitor_ = target_idx;
        update_ewmh_current_desktop();
        focus_floating_window(window_to_move);
        if (config_.focus.warp_cursor_on_monitor_change)
        {
            warp_to_monitor(monitors_[target_idx]);
        }

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

    // Update Client for O(1) lookup
    if (auto* client = get_client(window_to_move))
    {
        client->monitor = target_idx;
        client->workspace = target_monitor.current_workspace;
    }

    // Update EWMH desktop for moved window
    uint32_t desktop = get_ewmh_desktop_index(target_idx, target_monitor.current_workspace);
    if (is_client_sticky(window_to_move))
        ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
    else
        ewmh_.set_window_desktop(window_to_move, desktop);

    rearrange_monitor(focused_monitor());
    rearrange_monitor(target_monitor);

    focused_monitor_ = target_idx;
    update_ewmh_current_desktop();
    focus_window(window_to_move);
    if (config_.focus.warp_cursor_on_monitor_change)
    {
        warp_to_monitor(target_monitor);
    }

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

// ─────────────────────────────────────────────────────────────────────────────
// Client registry helpers
// ─────────────────────────────────────────────────────────────────────────────

Client* WindowManager::get_client(xcb_window_t window)
{
    auto it = clients_.find(window);
    return it != clients_.end() ? &it->second : nullptr;
}

Client const* WindowManager::get_client(xcb_window_t window) const
{
    auto it = clients_.find(window);
    return it != clients_.end() ? &it->second : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// State query helpers (prefer Client, fall back to legacy sets during migration)
// These will eventually just use clients_ once migration is complete.
// ─────────────────────────────────────────────────────────────────────────────

bool WindowManager::is_client_fullscreen(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->fullscreen;
    return false;  // Unmanaged windows are not fullscreen
}

bool WindowManager::is_client_iconic(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->iconic;
    return false;  // Unmanaged windows are not iconic
}

bool WindowManager::is_client_sticky(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->sticky;
    return false;  // Unmanaged windows are not sticky
}

bool WindowManager::is_client_above(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->above;
    return false;  // Unmanaged windows are not above
}

bool WindowManager::is_client_below(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->below;
    return false;  // Unmanaged windows are not below
}

bool WindowManager::is_client_maximized_horz(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->maximized_horz;
    return false;
}

bool WindowManager::is_client_maximized_vert(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->maximized_vert;
    return false;
}

bool WindowManager::is_client_shaded(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->shaded;
    return false;
}

bool WindowManager::is_client_modal(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->modal;
    return false;
}

bool WindowManager::is_client_skip_taskbar(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->skip_taskbar;
    return false;
}

bool WindowManager::is_client_skip_pager(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->skip_pager;
    return false;
}

bool WindowManager::is_client_demands_attention(xcb_window_t window) const
{
    if (auto const* c = get_client(window))
        return c->demands_attention;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Legacy floating window helpers (will be removed in Phase 6)
// ─────────────────────────────────────────────────────────────────────────────

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
    // O(1) lookup using Client registry
    if (auto const* c = get_client(window))
        return c->monitor;
    return std::nullopt;
}

std::optional<size_t> WindowManager::workspace_index_for_window(xcb_window_t window) const
{
    // O(1) lookup using Client registry
    if (auto const* c = get_client(window))
        return c->workspace;
    return std::nullopt;
}

/**
 * @brief Get the _NET_WM_DESKTOP value for a window.
 *
 * @return The desktop index, or nullopt if not set or if 0xFFFFFFFF (sticky).
 *         Use is_sticky_desktop() to check for sticky separately.
 */
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

/**
 * @brief Check if window requests sticky via _NET_WM_DESKTOP = 0xFFFFFFFF.
 *
 * EWMH specifies that _NET_WM_DESKTOP of 0xFFFFFFFF means the window should
 * be visible on all desktops (sticky).
 */
bool WindowManager::is_sticky_desktop(xcb_window_t window) const
{
    uint32_t desktop = 0;
    if (!xcb_ewmh_get_wm_desktop_reply(
            ewmh_.get(),
            xcb_ewmh_get_wm_desktop(ewmh_.get(), window),
            &desktop,
            nullptr
        ))
    {
        return false;
    }
    return desktop == 0xFFFFFFFF;
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

bool WindowManager::is_window_visible(xcb_window_t window) const
{
    if (showing_desktop_)
        return false;
    if (is_client_iconic(window))
        return false;

    if (auto const* floating_window = find_floating_window(window))
    {
        if (floating_window->monitor >= monitors_.size())
            return false;
        if (is_client_sticky(window))
            return true;
        return floating_window->workspace == monitors_[floating_window->monitor].current_workspace;
    }

    if (auto monitor_idx = monitor_index_for_window(window))
    {
        if (is_client_sticky(window))
            return true;
        auto workspace_idx = workspace_index_for_window(window);
        if (!workspace_idx)
            return false;
        return *workspace_idx == monitors_[*monitor_idx].current_workspace;
    }

    return false;
}

void WindowManager::restack_transients(xcb_window_t parent)
{
    if (parent == XCB_NONE)
        return;
    if (!is_window_visible(parent))
        return;

    for (auto const& floating_window : floating_windows_)
    {
        if (floating_window.transient_for != parent)
            continue;
        if (!is_window_visible(floating_window.id))
            continue;

        uint32_t values[2];
        values[0] = parent;
        values[1] = XCB_STACK_MODE_ABOVE;
        uint16_t mask = XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;
        xcb_configure_window(conn_.get(), floating_window.id, mask, values);
    }
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

        bool sticky = is_client_sticky(floating_window.id);
        if ((sticky || floating_window.workspace == monitor.current_workspace)
            && !is_client_iconic(floating_window.id))
        {
            // Configure BEFORE mapping so window appears at correct position
            if (is_client_fullscreen(floating_window.id))
            {
                apply_fullscreen_if_needed(floating_window.id);
            }
            else if (is_client_maximized_horz(floating_window.id)
                || is_client_maximized_vert(floating_window.id))
            {
                apply_maximized_geometry(floating_window.id);
            }
            else
            {
                apply_floating_geometry(floating_window);
            }
            xcb_map_window(conn_.get(), floating_window.id);
            if (floating_window.transient_for != XCB_NONE)
            {
                restack_transients(floating_window.transient_for);
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
    // Dock and desktop windows are never focus-eligible (per BEHAVIOR.md)
    if (auto* client = get_client(window))
    {
        if (client->kind == Client::Kind::Dock || client->kind == Client::Kind::Desktop)
            return false;
    }

    // Check ICCCM WM_HINTS.input hint
    xcb_icccm_wm_hints_t hints;
    if (!xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), window), &hints, nullptr))
        return true;

    if (!(hints.flags & XCB_ICCCM_WM_HINT_INPUT))
        return true;

    if (hints.input)
        return true;

    // If input=False, window can only receive focus via WM_TAKE_FOCUS
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

/**
 * @brief Send _NET_WM_SYNC_REQUEST to synchronize with client before resize.
 *
 * This notifies the client that the WM is about to resize the window and waits
 * (briefly) for the client to update its sync counter, indicating it's ready.
 *
 * LIMITATION: The current implementation blocks while waiting for the counter.
 * The timeout is kept short (50ms) to minimize impact on responsiveness.
 * A fully non-blocking implementation would require async state tracking.
 *
 * Many tiling WMs skip sync requests entirely since window geometry is WM-controlled.
 */
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

/**
 * @brief Wait for sync counter to reach expected value.
 *
 * WARNING: This function blocks with a busy-wait loop, repeatedly querying
 * the X server. The timeout is kept short to minimize impact.
 *
 * TODO: Implement async version using XSync alarms or event-driven waiting.
 */
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

/**
 * @brief Get the user interaction time for a window.
 *
 * EWMH specifies that _NET_WM_USER_TIME tracks the last user interaction time.
 * If _NET_WM_USER_TIME_WINDOW is set, we read the time from that window instead.
 * This is used for focus stealing prevention.
 */
uint32_t WindowManager::get_user_time(xcb_window_t window)
{
    if (net_wm_user_time_ == XCB_NONE)
        return 0;

    // Check if this window has a separate user time window
    xcb_window_t time_window = window;
    auto it = user_time_windows_.find(window);
    if (it != user_time_windows_.end() && it->second != XCB_NONE)
    {
        time_window = it->second;
    }

    auto cookie = xcb_get_property(conn_.get(), 0, time_window, net_wm_user_time_, XCB_ATOM_CARDINAL, 0, 1);
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
        clients_.erase(window);  // Remove from unified Client registry
        update_struts();
        rearrange_all_monitors();
        update_ewmh_client_list();
    }
}

void WindowManager::unmanage_desktop_window(xcb_window_t window)
{
    auto it = std::ranges::find(desktop_windows_, window);
    if (it != desktop_windows_.end())
    {
        desktop_windows_.erase(it);
        clients_.erase(window);  // Remove from unified Client registry
        update_ewmh_client_list();
    }
}

/**
 * @brief Unmap a window with WM tracking to distinguish from client-initiated unmaps.
 *
 * ICCCM requires window managers to distinguish between WM-initiated unmaps
 * (e.g., hiding windows during workspace switches) and client-initiated unmaps
 * (withdraw requests). This function tracks WM-initiated unmaps by incrementing
 * a counter before calling xcb_unmap_window. The counter is decremented in
 * the UnmapNotify handler, and if the count is positive, the unmap is ignored.
 *
 * To avoid counter leaks (incrementing without a matching UnmapNotify), we only
 * increment if the window is currently mapped (XCB_MAP_STATE_VIEWABLE).
 * Unmapping an already-unmapped window produces no UnmapNotify event.
 */
void WindowManager::wm_unmap_window(xcb_window_t window)
{
    // Only increment counter if window is currently viewable.
    // Unmapping an already-unmapped window produces no UnmapNotify,
    // which would leak the counter and cause future client unmaps to be ignored.
    auto attr_cookie = xcb_get_window_attributes(conn_.get(), window);
    auto* attr = xcb_get_window_attributes_reply(conn_.get(), attr_cookie, nullptr);
    if (attr)
    {
        bool viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;
        free(attr);
        if (viewable)
        {
            wm_unmapped_windows_[window] += 1;
            xcb_unmap_window(conn_.get(), window);
        }
        // Window already unmapped - no action needed
    }
    else
    {
        // Window might be destroyed - still try to unmap but don't track
        xcb_unmap_window(conn_.get(), window);
    }
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
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t max_x = 0;
    int32_t max_y = 0;
    if (!monitors_.empty())
    {
        min_x = monitors_[0].x;
        min_y = monitors_[0].y;
        max_x = static_cast<int32_t>(monitors_[0].x) + monitors_[0].width;
        max_y = static_cast<int32_t>(monitors_[0].y) + monitors_[0].height;
        for (auto const& monitor : monitors_)
        {
            min_x = std::min<int32_t>(min_x, monitor.x);
            min_y = std::min<int32_t>(min_y, monitor.y);
            max_x = std::max<int32_t>(max_x, static_cast<int32_t>(monitor.x) + monitor.width);
            max_y = std::max<int32_t>(max_y, static_cast<int32_t>(monitor.y) + monitor.height);
        }
    }

    desktop_origin_x_ = min_x;
    desktop_origin_y_ = min_y;

    uint32_t desktop_width = static_cast<uint32_t>(std::max<int32_t>(1, max_x - min_x));
    uint32_t desktop_height = static_cast<uint32_t>(std::max<int32_t>(1, max_y - min_y));
    ewmh_.set_desktop_geometry(desktop_width, desktop_height);

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
    ewmh_.set_desktop_viewport(monitors_, desktop_origin_x_, desktop_origin_y_);
    update_ewmh_workarea();
}

void WindowManager::update_ewmh_client_list()
{
    // Build client list sorted by mapping order using Client registry
    std::vector<std::pair<uint64_t, xcb_window_t>> ordered;
    ordered.reserve(clients_.size());
    for (auto const& [window, client] : clients_)
    {
        ordered.push_back({ client.order, window });
    }
    std::sort(ordered.begin(), ordered.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    std::vector<xcb_window_t> windows;
    windows.reserve(ordered.size());
    for (auto const& entry : ordered)
    {
        windows.push_back(entry.second);
    }
    ewmh_.update_client_list(windows);

    // Build stacking order from X server
    std::vector<xcb_window_t> stacking;
    stacking.reserve(windows.size());
    std::unordered_set<xcb_window_t> managed(windows.begin(), windows.end());

    auto cookie = xcb_query_tree(conn_.get(), conn_.screen()->root);
    auto* reply = xcb_query_tree_reply(conn_.get(), cookie, nullptr);
    if (reply)
    {
        int length = xcb_query_tree_children_length(reply);
        xcb_window_t* children = xcb_query_tree_children(reply);
        for (int i = 0; i < length; ++i)
        {
            if (managed.contains(children[i]))
            {
                stacking.push_back(children[i]);
                managed.erase(children[i]);
            }
        }
        free(reply);
    }

    // Append any windows not found in X tree (shouldn't happen, but be safe)
    if (!managed.empty())
    {
        for (auto const& entry : ordered)
        {
            if (managed.contains(entry.second))
                stacking.push_back(entry.second);
        }
    }

    ewmh_.update_client_list_stacking(stacking);
}

void WindowManager::update_ewmh_current_desktop()
{
    // Per-monitor workspaces: report the active monitor's current workspace only.
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
        int32_t offset_x = static_cast<int32_t>(area.x) - desktop_origin_x_;
        int32_t offset_y = static_cast<int32_t>(area.y) - desktop_origin_y_;
        offset_x = std::clamp<int32_t>(offset_x, 0, std::numeric_limits<int16_t>::max());
        offset_y = std::clamp<int32_t>(offset_y, 0, std::numeric_limits<int16_t>::max());
        area.x = static_cast<int16_t>(offset_x);
        area.y = static_cast<int16_t>(offset_y);
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
        if (is_client_sticky(window.id))
            continue;
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
