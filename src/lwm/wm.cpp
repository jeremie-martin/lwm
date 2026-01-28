#include "wm.hpp"
#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
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
    window_rules_.load_rules(config_.rules);
    detect_monitors();
    setup_ewmh();
    scan_existing_windows();
    run_autostart();
    keybinds_.grab_keys(conn_.screen()->root);
    update_ewmh_client_list();
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
        {
            LOG_ERROR("X connection error, shutting down");
            break;
        }
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
        uint16_t modifiers[] = { binding.modifier,
                                 static_cast<uint16_t>(binding.modifier | XCB_MOD_MASK_2),
                                 static_cast<uint16_t>(binding.modifier | XCB_MOD_MASK_LOCK),
                                 static_cast<uint16_t>(binding.modifier | XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK) };

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

void WindowManager::run_autostart()
{
    for (auto const& cmd : config_.autostart.commands)
    {
        std::string resolved = keybinds_.resolve_command(cmd, config_);
        LOG_INFO("Autostart: {}", resolved);
        launch_program(resolved);
    }
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
    std::string window_name = get_window_name(window);
    auto target = resolve_window_desktop(window);
    size_t target_monitor_idx = target ? target->first : focused_monitor_;
    size_t target_workspace_idx = target ? target->second : monitors_[target_monitor_idx].current_workspace;

    monitors_[target_monitor_idx].workspaces[target_workspace_idx].windows.push_back(window);

    // Populate unified Client registry
    {
        Client client;
        client.id = window;
        client.kind = Client::Kind::Tiled;
        client.monitor = target_monitor_idx;
        client.workspace = target_workspace_idx;
        client.name = window_name;
        client.wm_class = class_name;
        client.wm_class_name = instance_name;
        client.order = next_client_order_++;
        client.iconic = start_iconic;

        // Read and apply initial _NET_WM_STATE atoms (EWMH)
        xcb_ewmh_get_atoms_reply_t initial_state;
        if (xcb_ewmh_get_wm_state_reply(
                ewmh_.get(),
                xcb_ewmh_get_wm_state(ewmh_.get(), window),
                &initial_state,
                nullptr
            ))
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
    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE };
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
        ewmh->_NET_WM_ACTION_CLOSE,         ewmh->_NET_WM_ACTION_FULLSCREEN, ewmh->_NET_WM_ACTION_CHANGE_DESKTOP,
        ewmh->_NET_WM_ACTION_ABOVE,         ewmh->_NET_WM_ACTION_BELOW,      ewmh->_NET_WM_ACTION_MINIMIZE,
        ewmh->_NET_WM_ACTION_SHADE,         ewmh->_NET_WM_ACTION_STICK,      ewmh->_NET_WM_ACTION_MAXIMIZE_VERT,
        ewmh->_NET_WM_ACTION_MAXIMIZE_HORZ,
    };
    ewmh_.set_allowed_actions(window, actions);

    update_ewmh_client_list();

    // Check fullscreen BEFORE rearrange so window is properly excluded from tiling
    // and gets correct geometry when mapped. Other EWMH states are handled after.
    if (ewmh_.has_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN))
    {
        set_fullscreen(window, true);
    }

    keybinds_.grab_keys(window);

    // With off-screen visibility: map window once when managing, then hide if not on current workspace
    xcb_map_window(conn_.get(), window);

    if (!start_iconic)
    {
        if (is_workspace_visible(target_monitor_idx, target_workspace_idx))
        {
            rearrange_monitor(monitors_[target_monitor_idx]);
        }
        else
        {
            // Window is on a non-current workspace - hide it (move off-screen)
            hide_window(window);
        }
    }
    else
    {
        // Iconic windows should be hidden
        hide_window(window);
    }

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

    // Note: fullscreen is handled BEFORE rearrange_monitor to ensure correct geometry on map
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
    pending_kills_.erase(window);
    pending_pings_.erase(window);

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
                    workspace.focused_window = workspace.windows.empty() ? XCB_NONE : workspace.windows.back();
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

                conn_.flush();
                return;
            }
        }
    }
}

void WindowManager::set_fullscreen(xcb_window_t window, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return; // Only managed clients can be fullscreen

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
                else
                {
                    client->fullscreen_restore = std::nullopt;
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

        // Fullscreen supersedes maximized state - clear it to avoid confusing EWMH state
        if (client->maximized_horz || client->maximized_vert)
        {
            client->maximized_horz = false;
            client->maximized_vert = false;
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_HORZ, false);
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_VERT, false);
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
                // Sync Client.floating_geometry (authoritative)
                client->floating_geometry = floating_window->geometry;
                if (client->workspace == monitors_[client->monitor].current_workspace && !client->iconic)
                {
                    apply_floating_geometry(*floating_window);
                }
            }
        }
        else if (client->monitor < monitors_.size())
        {
            if (client->workspace == monitors_[client->monitor].current_workspace)
            {
                rearrange_monitor(monitors_[client->monitor]);
            }
        }

        client->fullscreen_restore = std::nullopt;
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);
    }

    update_ewmh_client_list();
    conn_.flush();
}

void WindowManager::set_window_above(xcb_window_t window, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return; // Only managed clients can have above state

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
    auto* client = get_client(window);
    if (!client)
        return; // Only managed clients can have below state

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
        return; // Only managed clients can be sticky

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
        uint32_t desktop = get_ewmh_desktop_index(client->monitor, client->workspace);
        ewmh_.set_window_desktop(window, desktop);
    }

    if (find_floating_window(window))
    {
        update_floating_visibility(client->monitor);
    }
    else if (auto* monitor = monitor_containing_window(window))
    {
        rearrange_monitor(*monitor);
    }

    update_ewmh_client_list();
    conn_.flush();
}

void WindowManager::set_window_maximized(xcb_window_t window, bool horiz, bool vert)
{
    auto* client = get_client(window);
    if (!client)
        return; // Only managed clients can be maximized

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
                    // Sync Client.floating_geometry (authoritative)
                    client->floating_geometry = floating_window->geometry;
                    if (client->workspace == monitors_[client->monitor].current_workspace && !client->iconic)
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
    if (client->monitor >= monitors_.size())
        return;

    Geometry base = floating_window->geometry;
    if (client->maximize_restore)
    {
        base = *client->maximize_restore;
    }

    Geometry area = monitors_[client->monitor].working_area();
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
    // Sync Client.floating_geometry (authoritative)
    client->floating_geometry = floating_window->geometry;
    if (client->workspace == monitors_[client->monitor].current_workspace && !client->iconic)
    {
        apply_floating_geometry(*floating_window);
    }
}

void WindowManager::set_window_shaded(xcb_window_t window, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return; // Only managed clients can be shaded

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
        return; // Only managed clients can be modal

    client->modal = enabled;

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MODAL, enabled);
    if (enabled)
    {
        set_window_above(window, true);
    }
    else
    {
        set_window_above(window, false);
    }
}

void WindowManager::set_client_skip_taskbar(xcb_window_t window, bool enabled)
{
    if (auto* c = get_client(window))
        c->skip_taskbar = enabled;
    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_TASKBAR, enabled);
}

void WindowManager::set_client_skip_pager(xcb_window_t window, bool enabled)
{
    if (auto* c = get_client(window))
        c->skip_pager = enabled;
    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_SKIP_PAGER, enabled);
}

void WindowManager::set_client_demands_attention(xcb_window_t window, bool enabled)
{
    if (auto* c = get_client(window))
        c->demands_attention = enabled;
    ewmh_.set_demands_attention(window, enabled);
}

void WindowManager::apply_fullscreen_if_needed(xcb_window_t window)
{
    LOG_DEBUG("apply_fullscreen_if_needed({:#x}) called", window);

    if (!is_client_fullscreen(window))
    {
        LOG_DEBUG("apply_fullscreen_if_needed({:#x}): NOT fullscreen, returning early", window);
        return;
    }
    if (is_client_iconic(window))
    {
        LOG_DEBUG("apply_fullscreen_if_needed({:#x}): is iconic, returning early", window);
        return;
    }

    auto const* client = get_client(window);
    if (!client)
    {
        LOG_DEBUG("apply_fullscreen_if_needed({:#x}): no client, returning early", window);
        return;
    }

    std::optional<size_t> monitor_idx = client->monitor;
    if (!monitor_idx || *monitor_idx >= monitors_.size())
    {
        LOG_DEBUG("apply_fullscreen_if_needed({:#x}): invalid monitor_idx, returning early", window);
        return;
    }
    if (client->workspace != monitors_[*monitor_idx].current_workspace)
    {
        LOG_DEBUG(
            "apply_fullscreen_if_needed({:#x}): workspace mismatch client->workspace={} vs "
            "monitor.current_workspace={}, returning early",
            window,
            client->workspace,
            monitors_[*monitor_idx].current_workspace
        );
        return;
    }
    LOG_DEBUG("apply_fullscreen_if_needed({:#x}): all checks passed, applying fullscreen geometry", window);

    Geometry area = fullscreen_geometry_for_window(window);

    // Send sync request before configure (matches Layout::configure_window pattern)
    send_sync_request(window, last_event_time_);

    uint32_t values[] = { static_cast<uint32_t>(area.x), static_cast<uint32_t>(area.y), area.width, area.height, 0 };
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
        | XCB_CONFIG_WINDOW_BORDER_WIDTH;
    xcb_configure_window(conn_.get(), window, mask, values);

    // Send synthetic ConfigureNotify so client knows its geometry immediately
    // This is critical for Electron/Chrome apps that need to know their size when fullscreened
    xcb_configure_notify_event_t ev = {};
    ev.response_type = XCB_CONFIGURE_NOTIFY;
    ev.event = window;
    ev.window = window;
    ev.x = area.x;
    ev.y = area.y;
    ev.width = static_cast<uint16_t>(area.width);
    ev.height = static_cast<uint16_t>(area.height);
    ev.border_width = 0;
    ev.above_sibling = XCB_NONE;
    ev.override_redirect = 0;
    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<char*>(&ev));

    uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
}

void WindowManager::set_fullscreen_monitors(xcb_window_t window, FullscreenMonitors const& monitors)
{
    // Fullscreen monitors are authoritative in Client
    if (auto* client = get_client(window))
        client->fullscreen_monitors = monitors;

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

    // Fullscreen monitors are authoritative in Client
    auto const* client = get_client(window);
    if (!client || !client->fullscreen_monitors)
    {
        if (auto monitor_idx = monitor_index_for_window(window))
            return monitors_[*monitor_idx].geometry();
        return monitors_[0].geometry();
    }

    std::vector<size_t> indices;
    indices.reserve(4);
    auto const& spec = *client->fullscreen_monitors;
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
    auto* client = get_client(window);
    if (!client)
        return; // Only managed clients can be iconified

    if (client->iconic)
        return;

    client->iconic = true;

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_ICONIC, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_HIDDEN, true);

    if (find_floating_window(window))
    {
        hide_window(window);
        update_floating_visibility(client->monitor);
    }
    else if (client->monitor < monitors_.size())
    {
        hide_window(window);
        rearrange_monitor(monitors_[client->monitor]);
    }

    if (active_window_ == window)
    {
        if (client->monitor == focused_monitor_ && client->workspace == monitors_[client->monitor].current_workspace)
        {
            focus_or_fallback(monitors_[client->monitor]);
        }
        else
        {
            clear_focus();
        }
    }

    conn_.flush();
}

void WindowManager::deiconify_window(xcb_window_t window, bool focus)
{
    auto* client = get_client(window);
    if (!client)
        return; // Only managed clients can be deiconified

    client->iconic = false;

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_NORMAL, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_HIDDEN, false);

    if (find_floating_window(window))
    {
        update_floating_visibility(client->monitor);
        if (focus && client->monitor == focused_monitor_
            && client->workspace == monitors_[client->monitor].current_workspace)
        {
            focus_floating_window(window);
        }
    }
    else if (client->monitor < monitors_.size())
    {
        rearrange_monitor(monitors_[client->monitor]);
        if (focus && client->monitor == focused_monitor_
            && client->workspace == monitors_[client->monitor].current_workspace)
        {
            focus_window(window);
        }
    }

    apply_fullscreen_if_needed(window);
    conn_.flush();
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
    // Find monitor index for logging
    size_t monitor_idx = 0;
    for (; monitor_idx < monitors_.size(); ++monitor_idx)
    {
        if (&monitors_[monitor_idx] == &monitor)
            break;
    }

    LOG_TRACE(
        "rearrange_monitor({}) called, current_ws={} windows_in_ws={}",
        monitor_idx,
        monitor.current_workspace,
        monitor.current().windows.size()
    );

    if (showing_desktop_)
    {
        LOG_TRACE("rearrange_monitor: showing_desktop_, unmapping all");
        for (xcb_window_t window : monitor.current().windows)
        {
            hide_window(window);
        }
        return;
    }

    // Only arrange current workspace - callers handle hiding old workspace
    std::vector<xcb_window_t> visible_windows;
    visible_windows.reserve(monitor.current().windows.size());
    std::vector<xcb_window_t> fullscreen_windows;
    std::unordered_set<xcb_window_t> seen;
    for (xcb_window_t window : monitor.current().windows)
    {
        if (is_client_iconic(window))
        {
            LOG_TRACE("rearrange_monitor: unmapping iconic window {:#x}", window);
            hide_window(window);
            continue;
        }
        // Fullscreen windows bypass tiling layout entirely
        if (is_client_fullscreen(window))
        {
            LOG_TRACE("rearrange_monitor: fullscreen window {:#x} excluded from tiling", window);
            fullscreen_windows.push_back(window);
            seen.insert(window);
            continue;
        }
        visible_windows.push_back(window);
        seen.insert(window);
    }

    for (auto const& workspace : monitor.workspaces)
    {
        for (xcb_window_t window : workspace.windows)
        {
            if (!is_client_sticky(window))
                continue;
            if (is_client_iconic(window))
            {
                LOG_TRACE("rearrange_monitor: unmapping iconic sticky window {:#x}", window);
                hide_window(window);
                continue;
            }
            // Sticky fullscreen windows also bypass tiling
            if (is_client_fullscreen(window))
            {
                if (!seen.contains(window))
                {
                    LOG_TRACE("rearrange_monitor: sticky fullscreen window {:#x} excluded from tiling", window);
                    fullscreen_windows.push_back(window);
                    seen.insert(window);
                }
                continue;
            }
            if (!seen.contains(window))
            {
                LOG_TRACE("rearrange_monitor: adding sticky window {:#x}", window);
                visible_windows.push_back(window);
                seen.insert(window);
            }
        }
    }

    LOG_DEBUG(
        "rearrange_monitor({}): arranging {} visible windows ({} fullscreen) on ws {}",
        monitor_idx,
        visible_windows.size(),
        fullscreen_windows.size(),
        monitor.current_workspace
    );

    for (size_t i = 0; i < visible_windows.size(); ++i)
    {
        LOG_DEBUG("rearrange_monitor: visible_windows[{}] = {:#x}", i, visible_windows[i]);
    }
    for (size_t i = 0; i < fullscreen_windows.size(); ++i)
    {
        LOG_DEBUG("rearrange_monitor: fullscreen_windows[{}] = {:#x}", i, fullscreen_windows[i]);
    }

    // Mark visible windows as shown before arranging (clears hidden flag)
    for (xcb_window_t window : visible_windows)
    {
        show_window(window);
    }

    layout_.arrange(visible_windows, monitor.working_area());

    // Handle fullscreen windows separately
    // With off-screen visibility, windows stay mapped so no "nudge resize" trick is needed.
    // The renderer never goes through unmap/remap cycles that cause redraw issues.
    for (xcb_window_t window : fullscreen_windows)
    {
        LOG_DEBUG("rearrange_monitor: handling fullscreen window {:#x}", window);

        // Mark as visible (window is already mapped, just restore from off-screen)
        show_window(window);

        // Configure to correct fullscreen geometry
        apply_fullscreen_if_needed(window);

        // Raise to top
        uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
        LOG_DEBUG("rearrange_monitor: raised fullscreen window {:#x} to top", window);
    }
    if (!fullscreen_windows.empty())
    {
        conn_.flush();
    }

    LOG_TRACE("rearrange_monitor: DONE");
}

void WindowManager::rearrange_all_monitors()
{
    for (auto& monitor : monitors_)
    {
        rearrange_monitor(monitor);
    }
    update_floating_visibility_all();
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
    // Search ALL tiled workspaces on ALL monitors (for tiled windows only)
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
// State query helpers - returns false for unmanaged windows
// ─────────────────────────────────────────────────────────────────────────────

#define DEFINE_CLIENT_STATE_QUERY(method, field)          \
    bool WindowManager::method(xcb_window_t window) const \
    {                                                     \
        if (auto const* c = get_client(window))           \
            return c->field;                              \
        return false;                                     \
    }

DEFINE_CLIENT_STATE_QUERY(is_client_fullscreen, fullscreen)
DEFINE_CLIENT_STATE_QUERY(is_client_iconic, iconic)
DEFINE_CLIENT_STATE_QUERY(is_client_sticky, sticky)
DEFINE_CLIENT_STATE_QUERY(is_client_above, above)
DEFINE_CLIENT_STATE_QUERY(is_client_below, below)
DEFINE_CLIENT_STATE_QUERY(is_client_maximized_horz, maximized_horz)
DEFINE_CLIENT_STATE_QUERY(is_client_maximized_vert, maximized_vert)
DEFINE_CLIENT_STATE_QUERY(is_client_shaded, shaded)
DEFINE_CLIENT_STATE_QUERY(is_client_modal, modal)
DEFINE_CLIENT_STATE_QUERY(is_client_skip_taskbar, skip_taskbar)
DEFINE_CLIENT_STATE_QUERY(is_client_skip_pager, skip_pager)
DEFINE_CLIENT_STATE_QUERY(is_client_demands_attention, demands_attention)

#undef DEFINE_CLIENT_STATE_QUERY

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
 * @brief Get raw _NET_WM_DESKTOP value for a window.
 * @return The desktop index, or nullopt if not set. 0xFFFFFFFF indicates sticky.
 */
std::optional<uint32_t> WindowManager::get_raw_window_desktop(xcb_window_t window) const
{
    uint32_t desktop = 0;
    if (!xcb_ewmh_get_wm_desktop_reply(ewmh_.get(), xcb_ewmh_get_wm_desktop(ewmh_.get(), window), &desktop, nullptr))
        return std::nullopt;
    return desktop;
}

std::optional<uint32_t> WindowManager::get_window_desktop(xcb_window_t window) const
{
    auto desktop = get_raw_window_desktop(window);
    return (desktop && *desktop != 0xFFFFFFFF) ? desktop : std::nullopt;
}

bool WindowManager::is_sticky_desktop(xcb_window_t window) const
{
    auto desktop = get_raw_window_desktop(window);
    return desktop && *desktop == 0xFFFFFFFF;
}

std::optional<std::pair<size_t, size_t>> WindowManager::resolve_window_desktop(xcb_window_t window) const
{
    if (config_.workspaces.count == 0)
        return std::nullopt;

    auto desktop = get_window_desktop(window);
    if (!desktop)
        return std::nullopt;

    auto indices = ewmh_policy::desktop_to_indices(*desktop, config_.workspaces.count);
    if (!indices)
        return std::nullopt;
    size_t monitor_idx = indices->first;
    size_t workspace_idx = indices->second;

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
    auto const* client = get_client(window);
    if (!client)
        return false;
    // Off-screen windows are not visible regardless of workspace state
    if (client->hidden)
        return false;
    return visibility_policy::is_window_visible(
        showing_desktop_,
        is_client_iconic(window),
        is_client_sticky(window),
        client->monitor,
        client->workspace,
        monitors_
    );
}

void WindowManager::restack_transients(xcb_window_t parent)
{
    if (parent == XCB_NONE)
        return;
    if (!is_window_visible(parent))
        return;

    for (auto const& fw : floating_windows_)
    {
        auto const* client = get_client(fw.id);
        if (!client || client->transient_for != parent)
            continue;
        if (!is_window_visible(fw.id))
            continue;

        uint32_t values[2];
        values[0] = parent;
        values[1] = XCB_STACK_MODE_ABOVE;
        uint16_t mask = XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;
        xcb_configure_window(conn_.get(), fw.id, mask, values);
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
    return visibility_policy::is_workspace_visible(showing_desktop_, monitor_idx, workspace_idx, monitors_);
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
 * @brief Send _NET_WM_SYNC_REQUEST to notify client before resize.
 *
 * This notifies the client that the WM is about to resize the window.
 * The sync request is sent without blocking - we don't wait for the client
 * to update its counter. This "fire and forget" approach is consistent with
 * how most tiling WMs handle sync requests, since window geometry is
 * WM-controlled and blocking would kill event loop responsiveness.
 *
 * Clients that support _NET_WM_SYNC_REQUEST will use the request to
 * synchronize their rendering, but we proceed with the configure regardless.
 */
void WindowManager::send_sync_request(xcb_window_t window, uint32_t timestamp)
{
    if (wm_protocols_ == XCB_NONE || net_wm_sync_request_ == XCB_NONE)
        return;

    auto* client = get_client(window);
    if (!client || client->sync_counter == 0)
        return;

    uint64_t value = ++client->sync_value;

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
    // Non-blocking: we don't wait for the client to update its counter
}

/**
 * @brief Wait for sync counter to reach expected value (unused).
 *
 * This function is preserved for potential future use but is not currently
 * called. The blocking busy-wait approach was replaced with non-blocking
 * sync requests to improve event loop responsiveness.
 */
bool WindowManager::wait_for_sync_counter(xcb_window_t window, uint64_t expected_value)
{
    auto* client = get_client(window);
    if (!client || client->sync_counter == 0)
        return false;

    xcb_sync_counter_t counter = client->sync_counter;
    auto deadline = std::chrono::steady_clock::now() + SYNC_WAIT_TIMEOUT;

    while (std::chrono::steady_clock::now() < deadline)
    {
        auto cookie = xcb_sync_query_counter(conn_.get(), counter);
        auto* reply = xcb_sync_query_counter_reply(conn_.get(), cookie, nullptr);
        if (!reply)
            return false;

        uint64_t current =
            (static_cast<uint64_t>(reply->counter_value.hi) << 32) | static_cast<uint64_t>(reply->counter_value.lo);
        free(reply);

        if (current >= expected_value)
        {
            client->sync_value = current;
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

    auto* client = get_client(window);
    if (!client)
        return;

    if (!supports_protocol(window, net_wm_sync_request_))
    {
        client->sync_counter = 0;
        client->sync_value = 0;
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
        client->sync_counter = 0;
        client->sync_value = 0;
        return;
    }

    client->sync_counter = counter;

    auto counter_cookie = xcb_sync_query_counter(conn_.get(), counter);
    auto* counter_reply = xcb_sync_query_counter_reply(conn_.get(), counter_cookie, nullptr);
    if (counter_reply)
    {
        uint64_t value = (static_cast<uint64_t>(counter_reply->counter_value.hi) << 32)
            | static_cast<uint64_t>(counter_reply->counter_value.lo);
        client->sync_value = value;
        free(counter_reply);
    }
    else
    {
        client->sync_value = 0;
    }
}

void WindowManager::update_fullscreen_monitor_state(xcb_window_t window)
{
    if (net_wm_fullscreen_monitors_ == XCB_NONE)
        return;

    auto* client = get_client(window);
    if (!client)
        return;

    xcb_ewmh_get_wm_fullscreen_monitors_reply_t reply;
    if (!xcb_ewmh_get_wm_fullscreen_monitors_reply(
            ewmh_.get(),
            xcb_ewmh_get_wm_fullscreen_monitors(ewmh_.get(), window),
            &reply,
            nullptr
        ))
    {
        client->fullscreen_monitors.reset();
        return;
    }

    FullscreenMonitors monitors;
    monitors.top = reply.top;
    monitors.bottom = reply.bottom;
    monitors.left = reply.left;
    monitors.right = reply.right;
    client->fullscreen_monitors = monitors;
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

    // Check if this window has a separate user time window (from Client.user_time_window)
    xcb_window_t time_window = window;
    if (auto const* client = get_client(window))
    {
        if (client->user_time_window != XCB_NONE)
            time_window = client->user_time_window;
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

    // Sync Client.name (authoritative)
    if (auto* client = get_client(window))
        client->name = name;
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
        clients_.erase(window); // Remove from unified Client registry
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
        clients_.erase(window); // Remove from unified Client registry
        update_ewmh_client_list();
    }
}

/**
 * @brief Hide a window by moving it off-screen (DWM-style visibility).
 *
 * This replaces the previous unmap-based approach. Windows stay mapped at all times
 * but are moved to x=-20000 when hidden. This resolves GPU-accelerated app redraw
 * issues (Chromium, Qt, Electron) that occur after unmap/remap cycles.
 *
 * Benefits:
 * - No UnmapNotify/MapNotify events, simplifying ICCCM compliance
 * - GPU-accelerated apps continue rendering and don't need reactivation
 * - Faster workspace switching (no window recreation overhead)
 *
 * @param window The window to hide
 */
void WindowManager::hide_window(xcb_window_t window)
{
    LOG_TRACE("hide_window({:#x}) called", window);

    auto* client = get_client(window);
    if (!client)
    {
        LOG_TRACE("hide_window({:#x}): no client, ignoring", window);
        return;
    }

    // Don't hide sticky windows (they're visible on all workspaces)
    if (client->sticky)
    {
        LOG_TRACE("hide_window({:#x}): sticky window, ignoring", window);
        return;
    }

    if (client->hidden)
    {
        LOG_TRACE("hide_window({:#x}): already hidden, skipping", window);
        return;
    }

    client->hidden = true;

    // Move window off-screen (preserve y coordinate for restore)
    uint32_t values[] = { static_cast<uint32_t>(OFF_SCREEN_X) };
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_X, values);

    LOG_TRACE("hide_window({:#x}): moved to x={}", window, OFF_SCREEN_X);
}

/**
 * @brief Show a previously hidden window by restoring its position.
 *
 * The window's geometry is restored via the normal layout/floating geometry
 * management. This function just clears the hidden flag - the caller is
 * responsible for configuring the correct geometry (via rearrange_monitor
 * or apply_floating_geometry).
 *
 * @param window The window to show
 */
void WindowManager::show_window(xcb_window_t window)
{
    LOG_TRACE("show_window({:#x}) called", window);

    auto* client = get_client(window);
    if (!client)
    {
        LOG_TRACE("show_window({:#x}): no client, ignoring", window);
        return;
    }

    if (!client->hidden)
    {
        LOG_TRACE("show_window({:#x}): not hidden, skipping", window);
        return;
    }

    client->hidden = false;
    LOG_TRACE("show_window({:#x}): marked as visible", window);
    // Caller must configure actual geometry via rearrange_monitor or apply_floating_geometry
}

void WindowManager::clear_all_borders()
{
    for (auto& monitor : monitors_)
    {
        for (auto& workspace : monitor.workspaces)
        {
            for (xcb_window_t window : workspace.windows)
            {
                xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &conn_.screen()->black_pixel);
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
