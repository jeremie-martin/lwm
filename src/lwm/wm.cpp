#include "wm.hpp"
#include "lwm/core/floating.hpp"
#include "lwm/core/focus.hpp"
#include "lwm/core/ipc.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb_icccm.h>

namespace lwm {

namespace {

constexpr auto KILL_TIMEOUT = std::chrono::seconds(5);

void ensure_property_change_mask(Connection& conn, xcb_window_t window)
{
    auto cookie = xcb_get_window_attributes(conn.get(), window);
    auto* reply = xcb_get_window_attributes_reply(conn.get(), cookie, nullptr);

    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    if (reply)
    {
        mask |= reply->your_event_mask;
        free(reply);
    }

    xcb_change_window_attributes(conn.get(), window, XCB_CW_EVENT_MASK, &mask);
}

void close_fd(int& fd)
{
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

void sigchld_handler(int /*sig*/)
{
    int saved_errno = errno;
    while (waitpid(-1, nullptr, WNOHANG) > 0)
    { }
    errno = saved_errno;
}

// Self-pipe for SIGHUP: handler writes a byte, main loop reads it.
int g_sighup_write_fd = -1;

void sighup_handler(int /*sig*/)
{
    int saved_errno = errno;
    if (g_sighup_write_fd >= 0)
    {
        char byte = 1;
        // write() is async-signal-safe; ignore failure (pipe full = already pending)
        (void)write(g_sighup_write_fd, &byte, 1);
    }
    errno = saved_errno;
}

void setup_signal_handlers()
{
    struct sigaction sa = {};
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);

    sa = {};
    sa.sa_handler = sighup_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &sa, nullptr);
}

}

WindowManager::WindowManager(Config config, std::string config_path)
    : config_(std::move(config))
    , conn_()
    , ewmh_(conn_)
    , keybinds_(conn_, config_)
    , layout_(config_.appearance, config_.layout)
    , config_path_(std::move(config_path))
{
    if (pipe2(signal_pipe_, O_CLOEXEC | O_NONBLOCK) == 0)
        g_sighup_write_fd = signal_pipe_[1];
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
    lwm_ipc_socket_ = intern_atom("_LWM_IPC_SOCKET");
    lwm_restart_client_ = intern_atom("_LWM_RESTART_CLIENT");
    lwm_restart_state_ = intern_atom("_LWM_RESTART_STATE");
    lwm_restart_tiled_order_ = intern_atom("_LWM_RESTART_TILED_ORDER");
    lwm_restart_floating_order_ = intern_atom("_LWM_RESTART_FLOATING_ORDER");
    lwm_restart_ratios_ = intern_atom("_LWM_RESTART_RATIOS");
    lwm_restart_scratchpad_name_ = intern_atom("_LWM_RESTART_SCRATCHPAD_NAME");
    lwm_restart_scratchpad_pool_ = intern_atom("_LWM_RESTART_SCRATCHPAD_POOL");
    lwm_window_class_ = intern_atom("_LWM_WINDOW_CLASS");
    {
        std::vector<xcb_atom_t> extra;
        if (net_wm_user_time_window_ != XCB_NONE)
            extra.push_back(net_wm_user_time_window_);
        if (net_wm_state_focused_ != XCB_NONE)
            extra.push_back(net_wm_state_focused_);
        if (lwm_window_class_ != XCB_NONE)
            extra.push_back(lwm_window_class_);
        if (!extra.empty())
            ewmh_.set_extra_supported_atoms(extra);
    }
    // Create cursors for tiled resize hover feedback
    {
        xcb_font_t font = xcb_generate_id(conn_.get());
        xcb_open_font(conn_.get(), font, 6, "cursor");

        cursor_default_ = xcb_generate_id(conn_.get());
        xcb_create_glyph_cursor(
            conn_.get(),
            cursor_default_,
            font,
            font,
            68,
            69,
            0,
            0,
            0,
            0xFFFF,
            0xFFFF,
            0xFFFF
        ); // left_ptr

        cursor_resize_h_ = xcb_generate_id(conn_.get());
        xcb_create_glyph_cursor(
            conn_.get(),
            cursor_resize_h_,
            font,
            font,
            108,
            109,
            0,
            0,
            0,
            0xFFFF,
            0xFFFF,
            0xFFFF
        ); // sb_h_double_arrow

        cursor_resize_v_ = xcb_generate_id(conn_.get());
        xcb_create_glyph_cursor(
            conn_.get(),
            cursor_resize_v_,
            font,
            font,
            116,
            117,
            0,
            0,
            0,
            0xFFFF,
            0xFFFF,
            0xFFFF
        ); // sb_v_double_arrow

        xcb_close_font(conn_.get(), font);

        set_root_cursor(cursor_default_);
    }
    window_rules_.load_rules(config_.rules);
    init_scratchpad_state();
    detect_monitors();
    setup_ewmh();
    setup_ipc();
    is_restart_ = restore_global_restart_state();
    scan_existing_windows();
    if (is_restart_)
    {
        restore_window_ordering();
        // Restore focus to the previously active window
        if (active_window_ != XCB_NONE && is_managed(active_window_))
        {
            focus_any_window(active_window_);
        }
        else if (!monitors_.empty())
        {
            focus_or_fallback(monitors_[focused_monitor_]);
        }
        flush_and_drain_crossing();
    }
    else
    {
        run_autostart();
    }
    // Restart properties are single-use. Clear them after both compatible
    // restores and fresh scans so stale state from an older schema cannot linger.
    clean_restart_properties();
    keybinds_.grab_keys(conn_.screen()->root);
    update_ewmh_client_list();
    flush_stacking_list();
    conn_.flush();
}

WindowManager::~WindowManager()
{
    if (cursor_default_ != XCB_NONE)
        xcb_free_cursor(conn_.get(), cursor_default_);
    if (cursor_resize_h_ != XCB_NONE)
        xcb_free_cursor(conn_.get(), cursor_resize_h_);
    if (cursor_resize_v_ != XCB_NONE)
        xcb_free_cursor(conn_.get(), cursor_resize_v_);
    cleanup_ipc();
    g_sighup_write_fd = -1;
    close_fd(signal_pipe_[0]);
    close_fd(signal_pipe_[1]);
}

RunResult WindowManager::run()
{
    LWM_ASSERT_INVARIANTS(clients_, monitors_, active_window_);
    int xfd = xcb_get_file_descriptor(conn_.get());

    // Poll fd index constants for fixed entries
    constexpr size_t POLL_SIGNAL = 1;
    constexpr size_t POLL_IPC = 2;

    std::vector<pollfd> poll_fds;
    bool connection_failed = false;

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

        if (auto deadline = ipc_.deadline(); deadline && (!next_deadline || *deadline < *next_deadline))
            next_deadline = *deadline;

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
        // Replies can pull events into XCB's queue without leaving the fd readable.
        while (auto* event = xcb_poll_for_queued_event(conn_.get()))
        {
            deferred_events_.push_back(*event);
            free(event);
        }
        if (!deferred_events_.empty())
            timeout_ms = 0;

        // Build poll array: X fd, signal pipe, IPC listener, pending client, subscribers
        poll_fds.clear();
        poll_fds.push_back({ .fd = xfd, .events = POLLIN, .revents = 0 });
        poll_fds.push_back({ .fd = signal_pipe_[0], .events = POLLIN, .revents = 0 });
        ipc_.append_poll_fds(poll_fds);

        int poll_result = poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), timeout_ms);
        if (poll_result > 0)
        {
            // SIGHUP: drain pipe and reload config
            if (poll_fds[POLL_SIGNAL].revents & POLLIN)
            {
                char buf[64];
                while (read(signal_pipe_[0], buf, sizeof(buf)) > 0)
                { }
                LOG_INFO("SIGHUP received, reloading config");
                auto result = reload_config();
                if (result)
                    LOG_INFO("Config reloaded successfully");
                else
                    LOG_ERROR("Config reload failed: {}", result.error());
                emit_config_reload_result(result, "sighup");
            }

            ipc_.dispatch(
                std::span(poll_fds).subspan(POLL_IPC),
                [this](std::string const& command)
                {
                    auto reply = run_ipc_command(command);
                    flush_stacking_list();
                    conn_.flush();
                    return reply;
                }
            );

            while (!deferred_events_.empty())
            {
                auto event = deferred_events_.front();
                deferred_events_.pop_front();
                handle_event(event);
            }

            while (auto event = xcb_poll_for_event(conn_.get()))
            {
                std::unique_ptr<xcb_generic_event_t, decltype(&free)> eventPtr(event, free);
                handle_event(*eventPtr);
            }
        }

        while (!deferred_events_.empty())
        {
            auto event = deferred_events_.front();
            deferred_events_.pop_front();
            handle_event(event);
        }

        ipc_.expire();

        handle_timeouts();
        if (monitors_dirty_)
        {
            monitors_dirty_ = false;
            handle_randr_screen_change();
        }
        LWM_ASSERT_INVARIANTS(clients_, monitors_, active_window_);
        flush_stacking_list();
        conn_.flush();

        if (xcb_connection_has_error(conn_.get()))
        {
            LOG_CRITICAL("X connection error, shutting down");
            connection_failed = true;
            break;
        }
    }

    if (connection_failed)
        return RunResult::Failed;
    if (restarting_)
        return RunResult::Restart;
    return RunResult::Exit;
}

void WindowManager::setup_ipc()
{
    ipc_.start(ipc::default_socket_path().string());
    if (lwm_ipc_socket_ != XCB_NONE)
        ipc::set_root_text_property(conn_.get(), conn_.screen()->root, lwm_ipc_socket_, utf8_string_, ipc_.path());
    conn_.flush();
}

void WindowManager::cleanup_ipc()
{
    ipc_.stop();
    if (conn_.get() && !xcb_connection_has_error(conn_.get()))
    {
        ipc::delete_root_property(conn_.get(), conn_.screen()->root, lwm_ipc_socket_);
        conn_.flush();
    }
}

void WindowManager::emit_config_reload_result(std::expected<void, std::string> const& result, char const* source)
{
    if (result)
        emit_event(
            Event_ConfigReload,
            std::string("{\"event\":\"config_reload\",\"success\":true,\"source\":\"") + source + "\"}"
        );
    else
        emit_event(
            Event_ConfigReload,
            std::string("{\"event\":\"config_reload\",\"success\":false,\"source\":\"") + source + "\",\"error\":\""
                + json_escape(result.error()) + "\"}"
        );
}

std::expected<void, std::string> WindowManager::reload_config()
{
    if (config_path_.empty())
        return std::unexpected("no config path is configured");

    if (!std::filesystem::exists(config_path_))
        return std::unexpected("config file does not exist: " + config_path_);

    auto loaded = load_config_result(config_path_);
    if (!loaded)
        return std::unexpected(loaded.error());

    return apply_config_reload(*loaded);
}

std::expected<void, std::string> WindowManager::validate_reload(Config const& config) const
{
    if (config.workspaces.count != config_.workspaces.count)
    {
        return std::unexpected("live reload of [workspaces].count is unsupported; restart required");
    }

    return {};
}

std::expected<void, std::string> WindowManager::apply_config_reload(Config config)
{
    if (auto validation = validate_reload(config); !validation)
        return std::unexpected(validation.error());

    WindowRules reloaded_rules;
    reloaded_rules.load_rules(config.rules);

    config_ = std::move(config);
    window_rules_ = std::move(reloaded_rules);

    keybinds_.reload(config_);
    init_mousebinds();
    grab_buttons();
    regrab_all_keys();

    // Rebuild scratchpad state: preserve claimed windows, update matchers
    {
        std::vector<NamedScratchpadState> old_states = std::move(named_scratchpads_);
        named_scratchpads_.clear();
        for (auto const& sp : config_.scratchpads)
        {
            NamedScratchpadState state{ sp.name };
            // Preserve existing window claim if scratchpad name survived reload
            for (auto& old : old_states)
            {
                if (old.name == sp.name)
                {
                    state.state = std::move(old.state);
                    break;
                }
            }
            named_scratchpads_.push_back(std::move(state));
        }
        rebuild_scratchpad_matchers();

        // Unhide windows orphaned by removed scratchpad names
        for (auto& [window, client] : clients_)
        {
            auto const* named = scratchpad_named(client);
            if (!named)
                continue;
            if (!find_named_scratchpad(named->name))
            {
                LOG_INFO("Scratchpad '{}' removed from config, restoring window {:#x}", named->name, window);
                client.scratchpad.reset();
                if (client.iconic)
                {
                    client.iconic = false;
                    set_iconic_state(window, false);
                }
            }
        }
    }
    update_ewmh_desktops();
    reapply_rules_to_existing_windows();
    rearrange_all_monitors();

    repair_focus_after_visibility_change(focused_monitor_);

    apply_appearance_reload();
    flush_and_drain_crossing();
    return {};
}

void WindowManager::regrab_all_keys()
{
    keybinds_.grab_keys(conn_.screen()->root);

    std::vector<std::pair<uint64_t, xcb_window_t>> ordered;
    ordered.reserve(clients_.size());
    for (auto const& [window, client] : clients_)
    {
        if (client.kind() == Client::Kind::Tiled || client.kind() == Client::Kind::Floating)
            ordered.push_back({ client.order, window });
    }

    std::sort(ordered.begin(), ordered.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    for (auto const& [order, window] : ordered)
    {
        (void)order;
        keybinds_.grab_keys(window);
    }
}

uint32_t WindowManager::border_width_for_client(Client const& client) const
{
    if (client.fullscreen || client.borderless)
        return 0U;
    return config_.appearance.border_width;
}

uint32_t WindowManager::border_color_for_client(Client const& client) const
{
    if (client.id == active_window_)
        return config_.appearance.border_color;
    if (client.urgency.active())
        return config_.appearance.urgent_border_color;
    return conn_.screen()->black_pixel;
}

bool WindowManager::should_apply_focus_border(Client const& client) const
{
    if (!focus_policy::should_apply_focus_border(client.fullscreen))
        return false;
    return border_width_for_client(client) > 0;
}

void WindowManager::apply_appearance_reload()
{
    for (auto const& [window, client] : clients_)
    {
        if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
            continue;

        uint32_t border_width = border_width_for_client(client);
        uint32_t border_color = border_color_for_client(client);

        xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &border_color);
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);
    }
}

void WindowManager::update_allowed_actions(Client const& client)
{
    xcb_ewmh_connection_t* ewmh = ewmh_.get();
    std::vector<xcb_atom_t> actions = {
        ewmh->_NET_WM_ACTION_CLOSE,
        ewmh->_NET_WM_ACTION_CHANGE_DESKTOP,
        ewmh->_NET_WM_ACTION_MINIMIZE,
        ewmh->_NET_WM_ACTION_STICK,
    };

    actions.push_back(ewmh->_NET_WM_ACTION_FULLSCREEN);
    actions.push_back(ewmh->_NET_WM_ACTION_ABOVE);
    actions.push_back(ewmh->_NET_WM_ACTION_BELOW);
    actions.push_back(ewmh->_NET_WM_ACTION_MAXIMIZE_VERT);
    actions.push_back(ewmh->_NET_WM_ACTION_MAXIMIZE_HORZ);

    if (client.kind() == Client::Kind::Floating)
    {
        actions.push_back(ewmh->_NET_WM_ACTION_MOVE);
        actions.push_back(ewmh->_NET_WM_ACTION_RESIZE);
    }

    ewmh_.set_allowed_actions(client.id, actions);
    publish_lwm_window_class(client);
}

void WindowManager::publish_lwm_window_class(Client const& client)
{
    if (lwm_window_class_ == XCB_NONE)
        return;
    char const* value = client_kind_str(client.kind());
    xcb_atom_t string_type = (utf8_string_ != XCB_NONE) ? utf8_string_ : XCB_ATOM_STRING;
    xcb_change_property(
        conn_.get(),
        XCB_PROP_MODE_REPLACE,
        client.id,
        lwm_window_class_,
        string_type,
        8,
        static_cast<uint32_t>(std::strlen(value)),
        value
    );
}

Geometry WindowManager::current_window_geometry(xcb_window_t window) const
{
    Geometry fallback = { 0, 0, 300, 200 };
    if (auto const* client = get_client(window); client && client->kind() == Client::Kind::Floating)
        fallback = floating_geometry(*client);

    auto geom_cookie = xcb_get_geometry(conn_.get(), window);
    auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
    if (!geom_reply)
        return fallback;

    Geometry geometry = {
        .x = geom_reply->x,
        .y = geom_reply->y,
        .width = static_cast<uint16_t>(std::max<uint16_t>(1, geom_reply->width)),
        .height = static_cast<uint16_t>(std::max<uint16_t>(1, geom_reply->height)),
    };
    free(geom_reply);
    return geometry;
}

void WindowManager::convert_window_to_floating(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client || client->kind() != Client::Kind::Tiled || client->monitor >= monitors_.size())
        return;

    std::optional<Geometry> prior_floating = prior_floating_geometry(*client);
    size_t monitor_idx = client->monitor;
    size_t workspace_idx = client->workspace;
    auto& ws = monitors_[monitor_idx].workspaces[workspace_idx];
    std::optional<SavedTilePos> saved_position;
    auto pos_it = ws.find_window(window);
    if (pos_it != ws.windows.end())
        saved_position = { static_cast<size_t>(std::distance(ws.windows.begin(), pos_it)), monitor_idx, workspace_idx };
    remove_tiled_from_workspace(*client, monitor_idx, workspace_idx);

    client->mru_order = next_mru_order_++;
    client->suppress_next_configure_request = false;

    Geometry geometry = prior_floating ? *prior_floating : current_window_geometry(window);
    if (!prior_floating && (client->hidden || geometry.x <= OFF_SCREEN_X / 2))
    {
        geometry = floating::place_floating(
            monitors_[monitor_idx].working_area(),
            geometry.width,
            geometry.height,
            std::nullopt
        );
    }
    set_floating_state(*client, geometry, saved_position);

    uint32_t values[] = { kManagedWindowEventMask };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    update_allowed_actions(*client);
}

void WindowManager::convert_window_to_tiled(xcb_window_t window, std::optional<Geometry> prior_floating)
{
    auto* client = get_client(window);
    if (!client || client->kind() != Client::Kind::Floating || client->monitor >= monitors_.size())
        return;

    auto saved_position = saved_tiled_pos(*client);
    set_tiled_state(*client, prior_floating);
    client->suppress_next_configure_request = false;
    size_t monitor_idx = client->monitor;
    size_t workspace_idx = std::min(client->workspace, monitors_[monitor_idx].workspaces.size() - 1);
    add_tiled_to_workspace(*client, monitor_idx, workspace_idx);
    // Restore original position when returning to the same workspace (e.g. type-change round-trip)
    // so existing windows keep their layout slots instead of being displaced by the re-entering window.
    if (saved_position && saved_position->monitor == monitor_idx && saved_position->workspace == workspace_idx)
    {
        auto& ws_wins = monitors_[monitor_idx].workspaces[workspace_idx].windows;
        size_t target_pos = saved_position->index;
        auto it = std::prev(ws_wins.end());  // window was just push_backed by add_tiled_to_workspace
        if (target_pos + 1 < ws_wins.size()) // only rotate if not already in the right place
            std::rotate(ws_wins.begin() + target_pos, it, it + 1);
    }

    uint32_t values[] = { kManagedWindowEventMask };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    update_allowed_actions(*client);
}

void WindowManager::toggle_window_float(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;
    if (client->fullscreen || client->iconic)
        return;
    if (client->kind() != Client::Kind::Tiled && client->kind() != Client::Kind::Floating)
        return;
    if (showing_desktop_)
        return;

    size_t monitor_idx = client->monitor;

    bool toggled_to_floating = client->kind() == Client::Kind::Tiled;
    if (toggled_to_floating)
    {
        convert_window_to_floating(window);
        client = get_client(window);
        if (!client)
            return;
    }
    else
    {
        Geometry prior_floating = client->maximize_restore.value_or(floating_geometry(*client));

        if (client->maximized_horz || client->maximized_vert)
        {
            client->maximized_horz = false;
            client->maximized_vert = false;
            client->maximize_restore = std::nullopt;
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_HORZ, false);
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_VERT, false);
        }

        convert_window_to_tiled(window, prior_floating);
        client = get_client(window);
        if (!client)
            return;
    }

    finalize_visibility_on_monitor(monitor_idx);
    if (toggled_to_floating)
    {
        client = get_client(window);
        if (client && client->kind() == Client::Kind::Floating && should_be_visible(*client) && !client->hidden
            && !client->fullscreen)
        {
            if (client->maximized_horz || client->maximized_vert)
                apply_maximized_geometry(*client);
            else
                apply_floating_geometry(*client);
            stacking_dirty_ = true;
        }
    }
    flush_and_drain_crossing();

    focus_any_window(window);
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
        xcb_randr_select_input(
            conn_.get(),
            conn_.screen()->root,
            XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE
                | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE
        );
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
        LOG_WARN("randr: get_screen_resources_current returned no reply, falling back to single monitor");
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
        {
            LOG_WARN("randr: get_output_info(output={:#x}) returned no reply, skipping", outputs[i]);
            continue;
        }
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

        if (!crtc_reply)
        {
            LOG_WARN(
                "randr: get_crtc_info(crtc={:#x}, output={}) returned no reply, skipping monitor",
                out_reply->crtc,
                output_name
            );
        }
        else if (crtc_reply->width == 0 || crtc_reply->height == 0)
        {
            LOG_WARN(
                "randr: crtc={:#x} (output={}) has zero dimensions ({}x{}), skipping monitor",
                out_reply->crtc,
                output_name,
                crtc_reply->width,
                crtc_reply->height
            );
        }
        else
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
    Workspace ws_template{};
    if (auto strategy = parse_layout_strategy(config_.layout.strategy))
        ws_template.layout_strategy = *strategy;

    monitor.workspaces.assign(config_.workspaces.count, ws_template);
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

    auto apply_restart_floating_state = [this](xcb_window_t window)
    {
        auto* client = get_client(window);
        if (!client || client->kind() != Client::Kind::Floating || client->hidden || !should_be_visible(*client))
            return;

        if (client->fullscreen)
            apply_fullscreen_if_needed(*client);
        else if (client->maximized_horz || client->maximized_vert)
            apply_maximized_geometry(*client);
        else
            apply_floating_geometry(*client);
    };

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

        auto initial = classify_managed_window(window);
        auto const& classification = initial.classification;

        switch (classification.kind)
        {
            case WindowClassification::Kind::Desktop:
                map_desktop_window(window);
                break;

            case WindowClassification::Kind::Dock:
                map_dock_window(window);
                break;

            case WindowClassification::Kind::Popup:
                break;

            case WindowClassification::Kind::Floating:
            {
                manage_floating_window(window, initial);
                if (auto* c = get_client(window))
                {
                    if (is_restart_)
                        apply_restart_client_state(window);
                    // Source bits were populated directly above; publish via the sync path.
                    if (c->urgency.active())
                        sync_client_urgency_state(*c);
                }
                if (is_restart_)
                    apply_restart_floating_state(window);
                else
                    reevaluate_managed_window(window);
                break;
            }

            case WindowClassification::Kind::Tiled:
            {
                manage_window(window, initial);
                if (auto* c = get_client(window))
                {
                    if (is_restart_)
                        apply_restart_client_state(window);
                    if (c->urgency.active())
                        sync_client_urgency_state(*c);
                }
                if (is_restart_)
                    apply_restart_floating_state(window);
                else
                    reevaluate_managed_window(window);
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
        LOG_INFO("Autostart: {}", cmd.describe());
        launch_program(cmd);
    }
}

void WindowManager::parse_initial_ewmh_state(Client& client)
{
    xcb_ewmh_get_atoms_reply_t initial_state;
    if (xcb_ewmh_get_wm_state_reply(
            ewmh_.get(),
            xcb_ewmh_get_wm_state(ewmh_.get(), client.id),
            &initial_state,
            nullptr
        ))
    {
        xcb_ewmh_connection_t* ewmh = ewmh_.get();
        for (uint32_t i = 0; i < initial_state.atoms_len; ++i)
        {
            xcb_atom_t state = initial_state.atoms[i];
            if (state == ewmh->_NET_WM_STATE_ABOVE)
            {
                client.layer_hint = LayerHint::Above;
                client.app_prefs.above = true;
            }
            else if (state == ewmh->_NET_WM_STATE_BELOW)
            {
                client.layer_hint = LayerHint::Below;
                client.app_prefs.below = true;
            }
            else if (state == ewmh->_NET_WM_STATE_STICKY)
                client.sticky = true;
            else if (state == ewmh->_NET_WM_STATE_MODAL)
                client.modal = true;
            else if (state == ewmh->_NET_WM_STATE_SKIP_TASKBAR)
            {
                client.skip_taskbar = true;
                client.app_prefs.skip_taskbar = true;
            }
            else if (state == ewmh->_NET_WM_STATE_SKIP_PAGER)
            {
                client.skip_pager = true;
                client.app_prefs.skip_pager = true;
            }
            else if (state == ewmh->_NET_WM_STATE_HIDDEN)
                client.iconic = true;
            else if (state == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION)
                client.urgency.add(UrgencySource::App);
        }
        if (client.app_prefs.above && client.app_prefs.below)
            client.app_prefs.below = false;
        xcb_ewmh_get_atoms_reply_wipe(&initial_state);
    }
}

void WindowManager::refresh_user_time_tracking_into(Client& client)
{
    xcb_window_t window = client.id;
    client.user_time_window = XCB_NONE;

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
                    client.user_time_window = time_window;
                    if (time_window != window)
                        ensure_property_change_mask(conn_, time_window);
                }
            }
            free(reply);
        }
    }

    client.user_time = get_user_time(window);
}

void WindowManager::refresh_user_time_tracking(xcb_window_t window)
{
    if (auto* client = get_client(window))
        refresh_user_time_tracking_into(*client);
}

void WindowManager::apply_post_manage_states(xcb_window_t window, bool has_transient)
{
    auto state_flags = ewmh_.get_window_state_flags(window);
    auto* client = get_client(window);
    if (!client)
        return;

    if (has_transient || state_flags.skip_taskbar)
    {
        set_client_skip_taskbar(*client, true);
    }
    if (has_transient || state_flags.skip_pager)
    {
        set_client_skip_pager(*client, true);
    }

    if (is_sticky_desktop(window))
    {
        set_window_sticky(*client, true);
    }
    else if (state_flags.sticky)
    {
        set_window_sticky(*client, true);
    }

    if (state_flags.modal)
    {
        set_window_modal(*client, true);
    }

    if (state_flags.above)
    {
        set_window_layer_hint(*client, LayerHint::Above);
    }
    else if (state_flags.below)
    {
        set_window_layer_hint(*client, LayerHint::Below);
    }
}

void WindowManager::manage_window(xcb_window_t window, ClassificationResult const& initial, bool start_iconic)
{
    auto target = resolve_window_desktop(window);
    if (target.kind == DesktopResolution::OutOfRange)
        LOG_WARN("manage_window({:#x}): _NET_WM_DESKTOP out of range, falling back to focused monitor", window);
    bool resolved = target.kind == DesktopResolution::Resolved;
    size_t target_monitor_idx = resolved ? target.monitor : focused_monitor_;
    size_t target_workspace_idx = resolved ? target.workspace : monitors_[target_monitor_idx].current_workspace;

    {
        Client client;
        client.id = window;
        set_tiled_state(client);
        client.monitor = target_monitor_idx;
        client.workspace = target_workspace_idx;
        client.name = initial.properties.title;
        client.wm_class = initial.properties.wm_class;
        client.wm_class_name = initial.properties.wm_class_name;
        client.order = next_client_order_++;
        client.iconic = start_iconic;
        client.ewmh_type = initial.properties.ewmh_type;
        parse_initial_ewmh_state(client);
        client.transient_for = initial.transient_for;
        client.desktop_pinned = resolved;

        clients_[window] = std::move(client);
    }
    cache_focus_hints(window);

    monitors_[target_monitor_idx].workspaces[target_workspace_idx].windows.push_back(window);

    refresh_user_time_tracking(window);

    // Note: We intentionally do NOT select STRUCTURE_NOTIFY on client windows.
    // We receive UnmapNotify/DestroyNotify via root's SubstructureNotifyMask.
    // Selecting STRUCTURE_NOTIFY on clients would cause duplicate UnmapNotify events,
    // leading to incorrect unmanagement of windows during workspace switches (ICCCM compliance).
    uint32_t values[] = { kManagedWindowEventMask };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);

    // Passive button grab for click-to-focus.  SYNC mode freezes the pointer so
    // we can focus and then REPLAY_POINTER to pass the click to the app (and to
    // any root grab for mod+button bindings).
    xcb_grab_button(
        conn_.get(),
        0,
        window,
        XCB_EVENT_MASK_BUTTON_PRESS,
        XCB_GRAB_MODE_SYNC,
        XCB_GRAB_MODE_ASYNC,
        XCB_NONE,
        XCB_NONE,
        XCB_BUTTON_INDEX_ANY,
        XCB_MOD_MASK_ANY
    );

    auto* client = get_client(window);
    if (!client)
        return;

    uint32_t border_width = border_width_for_client(*client);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { start_iconic ? WM_STATE_ICONIC : WM_STATE_NORMAL, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    if (start_iconic)
    {
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_HIDDEN, true);
    }

    update_sync_state(*client);
    update_fullscreen_monitor_state(*client);

    ewmh_.set_frame_extents(window, 0, 0, 0, 0);
    uint32_t desktop = get_ewmh_desktop_index(target_monitor_idx, target_workspace_idx);
    ewmh_.set_window_desktop(window, desktop);

    update_allowed_actions(*client);

    update_ewmh_client_list();

    // Fetch all EWMH state flags in a single round-trip
    auto manage_state_flags = ewmh_.get_window_state_flags(window);

    // Check fullscreen BEFORE rearrange so window is properly excluded from tiling
    // and gets correct geometry when mapped. Other EWMH states are handled after.
    if (manage_state_flags.fullscreen)
    {
        set_fullscreen(*client, true);
    }

    keybinds_.grab_keys(window);

    // With off-screen visibility: map window once when managing, then let
    // visibility reconciliation decide whether it should be hidden or shown.
    xcb_map_window(conn_.get(), window);

    finalize_visibility_on_monitor(target_monitor_idx);

    apply_post_manage_states(window, false);

    // Maximized states are geometry-affecting, applied post-map for tiled windows
    // (for floating, these are applied pre-map inline in manage_floating_window)
    if (manage_state_flags.maximized_horz || manage_state_flags.maximized_vert)
    {
        set_window_maximized(*client, manage_state_flags.maximized_horz, manage_state_flags.maximized_vert);
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

    auto const* client = get_client(window);
    if (!client)
        return;

    // Tracking state that remains outside Client
    pending_kills_.erase(window);

    size_t mon_idx = client->monitor;
    size_t ws_idx = client->workspace;
    bool was_active = (active_window_ == window);

    if (mon_idx < monitors_.size() && ws_idx < monitors_[mon_idx].workspaces.size())
    {
        auto& monitor = monitors_[mon_idx];
        bool const focus_fallback_if_active = ws_idx == monitor.current_workspace && mon_idx == focused_monitor_;
        workspace_policy::remove_tiled_window(
            monitor.workspaces[ws_idx],
            window,
            [this](xcb_window_t w) { return window_is_iconic(w); }
        );
        clients_.erase(window);
        update_ewmh_client_list();

        finalize_removed_window_after_unmanage(mon_idx, was_active, focus_fallback_if_active);
        return;
    }

    // Orphan path: client had stale monitor/workspace indices (e.g. after a
    // RandR shrink between manage and destroy). Still refresh EWMH so pagers
    // drop the dead window, and clear focus if the orphan was active.
    clients_.erase(window);
    update_ewmh_client_list();
    finalize_removed_window_after_unmanage(mon_idx, was_active, /*focus_fallback_if_active=*/false);
}

void WindowManager::clear_fullscreen_state(Client& client)
{
    if (!client.fullscreen)
        return;

    client.fullscreen = false;
    ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_FULLSCREEN, false);

    if (client.kind() == Client::Kind::Floating && client.fullscreen_restore)
        floating_geometry(client) = *client.fullscreen_restore;

    client.fullscreen_restore = std::nullopt;
    LayerHint restore_layer_hint = client.fullscreen_restore_layer_hint.value_or(LayerHint::Normal);
    client.fullscreen_restore_layer_hint = std::nullopt;
    uint32_t border_width = border_width_for_client(client);
    xcb_configure_window(conn_.get(), client.id, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);

    if (client.app_prefs.above)
        restore_layer_hint = LayerHint::Above;
    else if (client.app_prefs.below)
        restore_layer_hint = LayerHint::Below;

    if (client.layer_hint != restore_layer_hint)
        set_window_layer_hint(client, restore_layer_hint);

    if (client.kind() == Client::Kind::Floating && should_be_visible(client) && !client.hidden
        && !is_suppressed_by_fullscreen(client))
    {
        apply_floating_geometry(client);
    }
}

void WindowManager::set_fullscreen(Client& client, bool enabled)
{
    if (!enabled && !client.fullscreen)
        return;

    if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
    {
        LOG_WARN("set_fullscreen({:#x}): rejected invalid client state", client.id);
        // Echo the effective state so the requesting client receives a
        // PropertyNotify reflecting reality, rather than silently dropping
        // the _NET_WM_STATE request.
        ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_FULLSCREEN, client.fullscreen);
        conn_.flush();
        return;
    }

    if (enabled)
    {
        if (!client.fullscreen && client.kind() == Client::Kind::Floating)
        {
            client.fullscreen_restore = floating_geometry(client);
        }
        if (!client.fullscreen)
            client.fullscreen_restore_layer_hint = client.layer_hint;

        client.fullscreen = true;

        if (client.layer_hint != LayerHint::Normal)
            set_window_layer_hint(client, LayerHint::Normal);

        // Fullscreen supersedes maximized state - clear it to avoid confusing EWMH state
        if (client.maximized_horz || client.maximized_vert)
        {
            client.maximized_horz = false;
            client.maximized_vert = false;
            ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_HORZ, false);
            ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_VERT, false);
        }

        ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_FULLSCREEN, true);
    }
    else
    {
        clear_fullscreen_state(client);
    }

    finalize_visibility_on_monitor(client.monitor, enabled ? client.id : XCB_NONE);
    if (client.kind() == Client::Kind::Floating)
        apply_visible_floating_geometry(client);

    auto const* active = get_client(active_window_);
    bool const active_blocks_focus = active_window_ == XCB_NONE || (active && is_suppressed_by_fullscreen(*active));
    if (client.monitor < monitors_.size() && client.monitor == focused_monitor_ && active_blocks_focus)
        focus_or_fallback(monitors_[client.monitor], false);

    flush_and_drain_crossing();
}

void WindowManager::set_window_borderless(Client& client, bool enabled)
{
    client.borderless = enabled;
    uint32_t border_width = border_width_for_client(client);
    xcb_configure_window(conn_.get(), client.id, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);
    uint32_t color = (border_width > 0) ? border_color_for_client(client) : conn_.screen()->black_pixel;
    xcb_change_window_attributes(conn_.get(), client.id, XCB_CW_BORDER_PIXEL, &color);
    update_allowed_actions(client);
    conn_.flush();
}

void WindowManager::set_window_layer_hint(Client& client, LayerHint hint)
{
    auto* ewmh = ewmh_.get();

    LayerHint old = client.layer_hint;
    bool wants_above = hint == LayerHint::Above;
    bool wants_below = hint == LayerHint::Below;
    auto state_flags = ewmh_.get_window_state_flags(client.id);
    bool property_matches_hint = state_flags.above == wants_above && state_flags.below == wants_below;
    if (old == hint && property_matches_hint)
        return;

    client.layer_hint = hint;
    ewmh_.set_window_state(client.id, ewmh->_NET_WM_STATE_ABOVE, false);
    ewmh_.set_window_state(client.id, ewmh->_NET_WM_STATE_BELOW, false);
    if (wants_above)
        ewmh_.set_window_state(client.id, ewmh->_NET_WM_STATE_ABOVE, true);
    else if (wants_below)
        ewmh_.set_window_state(client.id, ewmh->_NET_WM_STATE_BELOW, true);

    if (old != hint)
        stacking_dirty_ = true;
    conn_.flush();
}

void WindowManager::set_window_sticky(Client& client, bool enabled)
{
    if (enabled)
    {
        client.desktop_pinned = false;
        client.sticky = true;
        ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_STICKY, true);
        ewmh_.set_window_desktop(client.id, 0xFFFFFFFF);
    }
    else
    {
        client.sticky = false;
        ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_STICKY, false);
        uint32_t desktop = get_ewmh_desktop_index(client.monitor, client.workspace);
        ewmh_.set_window_desktop(client.id, desktop);
    }

    size_t preferred_monitor = client.monitor;
    finalize_visibility_on_monitor(client.monitor);

    if (client.id == active_window_)
        repair_focus_after_visibility_change(preferred_monitor, false);

    flush_and_drain_crossing();
}

void WindowManager::set_window_maximized(Client& client, bool horiz, bool vert)
{
    client.maximized_horz = horiz;
    client.maximized_vert = vert;

    ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_HORZ, horiz);
    ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_VERT, vert);

    if (!client.fullscreen)
    {
        if (!horiz && !vert)
        {
            if (client.maximize_restore)
            {
                if (client.kind() == Client::Kind::Floating)
                {
                    floating_geometry(client) = *client.maximize_restore;
                    if (should_be_visible(client) && !client.hidden)
                    {
                        apply_floating_geometry(client);
                    }
                }
                client.maximize_restore = std::nullopt;
            }
        }
        else if (client.kind() == Client::Kind::Floating)
        {
            if (!client.maximize_restore)
            {
                client.maximize_restore = floating_geometry(client);
            }
            apply_maximized_geometry(client);
        }
    }

    conn_.flush();
}

void WindowManager::apply_maximized_geometry(Client& client)
{
    if (client.kind() != Client::Kind::Floating || client.monitor >= monitors_.size())
        return;

    Geometry base = floating_geometry(client);
    if (client.maximize_restore)
    {
        base = *client.maximize_restore;
    }

    Geometry area = monitors_[client.monitor].working_area();
    if (client.maximized_horz)
    {
        base.x = area.x;
        base.width = area.width;
    }
    if (client.maximized_vert)
    {
        base.y = area.y;
        base.height = area.height;
    }

    floating_geometry(client) = base;
    if (should_be_visible(client) && !client.hidden)
    {
        apply_floating_geometry(client);
    }
}

void WindowManager::set_window_modal(Client& client, bool enabled)
{
    client.modal = enabled;

    ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_MODAL, enabled);
    if (enabled)
    {
        set_window_layer_hint(client, LayerHint::Above);
    }
    else if (client.layer_hint == LayerHint::Above)
    {
        set_window_layer_hint(client, LayerHint::Normal);
    }
}

void WindowManager::set_client_skip_taskbar(Client& client, bool enabled)
{
    client.skip_taskbar = enabled;
    ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_SKIP_TASKBAR, enabled);
}

void WindowManager::set_client_skip_pager(Client& client, bool enabled)
{
    client.skip_pager = enabled;
    ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_SKIP_PAGER, enabled);
}

void WindowManager::sync_client_urgency_state(Client& client)
{
    bool const enabled = client.urgency.active();
    ewmh_.set_window_state(client.id, ewmh_.get()->_NET_WM_STATE_DEMANDS_ATTENTION, enabled);
    if (!enabled)
        client.ignore_next_wm_hints_urgency_echo = false;

    // Our own WM_HINTS write echoes back as PropertyNotify. If WM is the sole
    // source, suppress that echo so it isn't reclassified as app-originated.
    bool const arm_echo =
        enabled && client.urgency.has(UrgencySource::WmInitiated) && !client.urgency.has(UrgencySource::App);

    // Sync ICCCM WM_HINTS urgency flag so panels that check WM_HINTS (e.g. polybar
    // xworkspaces) also see the urgency state.
    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), client.id), &hints, nullptr))
    {
        bool const hints_urgent = (hints.flags & XUrgencyHint) != 0;
        if (hints_urgent != enabled)
        {
            if (enabled)
                hints.flags |= XUrgencyHint;
            else
                hints.flags &= ~XUrgencyHint;
            if (arm_echo)
                client.ignore_next_wm_hints_urgency_echo = true;
            xcb_icccm_set_wm_hints(conn_.get(), client.id, &hints);
        }
    }
    else if (enabled)
    {
        xcb_icccm_wm_hints_t new_hints = {};
        new_hints.flags = XUrgencyHint;
        if (arm_echo)
            client.ignore_next_wm_hints_urgency_echo = true;
        xcb_icccm_set_wm_hints(conn_.get(), client.id, &new_hints);
    }

    if (client.id != active_window_ && border_width_for_client(client) > 0)
    {
        uint32_t color = border_color_for_client(client);
        xcb_change_window_attributes(conn_.get(), client.id, XCB_CW_BORDER_PIXEL, &color);
    }

    // Re-publish client list so EWMH panels (e.g. polybar) re-check urgency.
    update_ewmh_client_list();
    conn_.flush();
}

void WindowManager::set_client_urgency(Client& client, UrgencySource source, bool enabled)
{
    bool const changed = enabled ? client.urgency.add(source) : client.urgency.remove(source);
    if (!changed)
        return;

    sync_client_urgency_state(client);
}

void WindowManager::clear_client_urgency(Client& client)
{
    if (!client.urgency.clear())
        return;

    sync_client_urgency_state(client);
}

void WindowManager::apply_fullscreen_if_needed(Client& client)
{
    if (!client.fullscreen)
    {
        LOG_DEBUG("apply_fullscreen_if_needed({:#x}): NOT fullscreen, returning early", client.id);
        return;
    }
    if (is_suppressed_by_fullscreen(client))
    {
        LOG_DEBUG("apply_fullscreen_if_needed({:#x}): suppressed by fullscreen owner, returning early", client.id);
        return;
    }
    if (client.iconic)
    {
        LOG_DEBUG("apply_fullscreen_if_needed({:#x}): is iconic, returning early", client.id);
        return;
    }

    if (client.monitor >= monitors_.size())
    {
        LOG_DEBUG("apply_fullscreen_if_needed({:#x}): invalid monitor_idx, returning early", client.id);
        return;
    }

    if (!client.sticky && client.workspace != monitors_[client.monitor].current_workspace)
    {
        LOG_DEBUG(
            "apply_fullscreen_if_needed({:#x}): workspace mismatch client->workspace={} vs "
            "monitor.current_workspace={}, returning early",
            client.id,
            client.workspace,
            monitors_[client.monitor].current_workspace
        );
        return;
    }
    LOG_DEBUG("apply_fullscreen_if_needed({:#x}): all checks passed, applying fullscreen geometry", client.id);

    Geometry area = fullscreen_geometry_for_client(client);

    apply_geometry(client, area, 0);
}

void WindowManager::set_fullscreen_monitors(Client& client, FullscreenMonitors const& monitors)
{
    client.fullscreen_monitors = monitors;

    if (net_wm_fullscreen_monitors_ != XCB_NONE)
    {
        xcb_ewmh_set_wm_fullscreen_monitors(
            ewmh_.get(),
            client.id,
            monitors.top,
            monitors.bottom,
            monitors.left,
            monitors.right
        );
    }

    if (client.fullscreen)
    {
        apply_fullscreen_if_needed(client);
        conn_.flush();
    }
}

Geometry WindowManager::fullscreen_geometry_for_client(Client const& client) const
{
    if (monitors_.empty())
        return {};

    auto fallback_monitor_geometry = [&]() -> Geometry
    {
        if (client.monitor < monitors_.size())
            return monitors_[client.monitor].geometry();
        return monitors_[0].geometry();
    };

    if (!client.fullscreen_monitors)
        return fallback_monitor_geometry();

    std::vector<size_t> indices;
    indices.reserve(4);
    auto const& spec = *client.fullscreen_monitors;
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
        return fallback_monitor_geometry();

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

void WindowManager::set_iconic_state(xcb_window_t window, bool iconic)
{
    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { iconic ? WM_STATE_ICONIC : WM_STATE_NORMAL, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }
    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_HIDDEN, iconic);
}

void WindowManager::iconify_window(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;

    if (client->iconic)
        return;

    if (client->kind() != Client::Kind::Tiled && client->kind() != Client::Kind::Floating)
    {
        LOG_WARN("iconify_window({:#x}): rejected invalid client state", window);
        return;
    }

    bool const was_visible = should_be_visible(*client);
    client->iconic = true;
    if (client->kind() == Client::Kind::Tiled)
    {
        auto& workspace = monitors_[client->monitor].workspaces[client->workspace];
        workspace_policy::fixup_workspace_focus(
            workspace,
            window,
            [this](xcb_window_t candidate) { return window_is_iconic(candidate); }
        );
    }
    set_iconic_state(window, true);

    finalize_visibility_on_monitor(client->monitor);

    if (active_window_ == window)
    {
        if (client->monitor == focused_monitor_ && was_visible)
            focus_or_fallback(monitors_[client->monitor]);
        else
            clear_focus();
    }

    flush_and_drain_crossing();
}

void WindowManager::deiconify_window(xcb_window_t window, bool focus)
{
    auto* client = get_client(window);
    if (!client)
        return;

    if (client->kind() != Client::Kind::Tiled && client->kind() != Client::Kind::Floating)
    {
        LOG_WARN("deiconify_window({:#x}): rejected invalid client state", window);
        return;
    }

    client->iconic = false;
    set_iconic_state(window, false);

    finalize_visibility_on_monitor(client->monitor, client->fullscreen ? window : XCB_NONE);

    if ((focus || client->fullscreen) && client->monitor == focused_monitor_ && should_be_visible(*client))
        focus_any_window(window);

    apply_fullscreen_if_needed(*client);
    flush_and_drain_crossing();
}

// A ping reply cancels the pending force-kill even if the client stays open
// (for example, to show a save dialog). See X11.md for close behavior.
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

    // Window doesn't support graceful close - force kill immediately
    xcb_kill_client(conn_.get(), window);
    conn_.flush();
}

void WindowManager::rearrange_monitor(Monitor& monitor, bool geometry_only)
{
    size_t monitor_idx = monitor_index(monitor);

    LOG_TRACE(
        "rearrange_monitor({}) called, current_ws={} windows_in_ws={}",
        monitor_idx,
        monitor.current_workspace,
        monitor.current().windows.size()
    );

    // Membership is unique by invariant. Visit the current workspace first,
    // then visible sticky clients in the other workspaces, preserving layout order.
    std::vector<xcb_window_t> visible_windows;
    visible_windows.reserve(monitor.current().windows.size());
    std::vector<xcb_window_t> visible_fullscreen_windows;
    auto collect = [&](Workspace const& workspace, bool current)
    {
        for (xcb_window_t window : workspace.windows)
        {
            auto const& client = require_client(window);
            if (client.hidden || (!current && !client.sticky))
                continue;
            (client.fullscreen ? visible_fullscreen_windows : visible_windows).push_back(window);
        }
    };
    collect(monitor.current(), true);
    for (size_t i = 0; i < monitor.workspaces.size(); ++i)
        if (i != monitor.current_workspace)
            collect(monitor.workspaces[i], false);

    auto& ws = monitor.current();

    auto slots = layout_.arrange(visible_windows.size(), monitor.working_area(), ws.layout_strategy, ws.split_ratios);
    for (size_t i = 0; i < visible_windows.size(); ++i)
    {
        auto& client = require_client(visible_windows[i]);
        if (!geometry_only || client.tiled_geometry != slots[i])
            apply_geometry(client, slots[i], border_width_for_client(client));
        client.tiled_geometry = slots[i];
    }

    // Apply fullscreen geometry for visible fullscreen tiled windows
    for (xcb_window_t window : visible_fullscreen_windows)
    {
        LOG_DEBUG("rearrange_monitor: applying fullscreen geometry for {:#x}", window);
        if (auto* client = get_client(window))
            apply_fullscreen_if_needed(*client);
    }

    if (!geometry_only)
        stacking_dirty_ = true;
    conn_.flush();

    LOG_TRACE("rearrange_monitor: DONE");
}

void WindowManager::rearrange_all_monitors()
{
    for (size_t monitor_idx = 0; monitor_idx < monitors_.size(); ++monitor_idx)
        reconcile_visibility_for_monitor(monitor_idx);

    for (size_t monitor_idx = 0; monitor_idx < monitors_.size(); ++monitor_idx)
        rearrange_monitor(monitors_[monitor_idx]);
}

bool WindowManager::launch_program(CommandConfig const& command)
{
    if (command.empty())
        return false;
    std::vector<char*> argv;
    if (command.kind == CommandConfig::Kind::Shell)
        argv = { const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), const_cast<char*>(command.shell.c_str()) };
    else
        for (auto const& arg : command.argv)
            argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    // posix_spawn reports exec failure to the parent. Owned WM descriptors are
    // CLOEXEC; stderr is inherited so the application can report its own errors.
    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (!error)
    {
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
        pid_t child;
        if (!error)
            error = posix_spawnp(&child, argv.front(), nullptr, &attributes, argv.data(), environ);
        posix_spawnattr_destroy(&attributes);
    }
    if (error)
        LOG_ERROR("Cannot launch '{}': {}", command.describe(), std::strerror(error));
    return error == 0;
}

bool WindowManager::adjust_master_ratio(double delta)
{
    if (focused_monitor_ >= monitors_.size())
        return false;

    auto& ws = focused_monitor().current();
    double min_ratio = config_.layout.min_ratio;

    SplitAddress root_addr{ 0 };

    double current = config_.layout.default_ratio;
    if (auto it = ws.split_ratios.find(root_addr); it != ws.split_ratios.end())
        current = it->second;

    double clamped = std::clamp(current + delta, min_ratio, 1.0 - min_ratio);
    if (clamped == current)
        return false;
    ws.split_ratios[root_addr] = clamped;

    rearrange_monitor(focused_monitor(), true);
    return true;
}

void WindowManager::swap_focused_tiled(int offset)
{
    if (focused_monitor_ >= monitors_.size())
        return;
    auto& ws = focused_monitor().current();
    auto it = ws.find_window(ws.focused_window);
    if (it == ws.windows.end())
        return;

    size_t n = ws.windows.size();
    if (n < 2)
        return;

    size_t idx = static_cast<size_t>(std::distance(ws.windows.begin(), it));
    int wrapped = static_cast<int>(idx) + offset;
    int signed_n = static_cast<int>(n);
    int other_signed = ((wrapped % signed_n) + signed_n) % signed_n;
    size_t other = static_cast<size_t>(other_signed);
    if (other == idx)
        return;

    // In Monocle every slot has the same content rect, so swapping positions
    // produces no visible change. Focus the adjacent tiled slot directly so
    // floating windows and MRU ordering cannot alter the requested target.
    if (ws.layout_strategy == LayoutStrategy::Monocle)
    {
        xcb_window_t target = ws.windows[other];
        focus_any_window(target);
        return;
    }

    std::swap(ws.windows[idx], ws.windows[other]);
    rearrange_monitor(focused_monitor(), true);
}

void WindowManager::reset_split_ratio(SplitAddress address, size_t monitor_idx)
{
    if (monitor_idx >= monitors_.size())
        return;
    auto& ws = monitors_[monitor_idx].current();
    if (ws.split_ratios.erase(address) > 0)
        rearrange_monitor(monitors_[monitor_idx], true);
}

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

Client& WindowManager::require_client(xcb_window_t window)
{
    auto it = clients_.find(window);
    if (it == clients_.end())
    {
        LOG_CRITICAL("require_client: window {:#x} not in clients registry", window);
        std::abort();
    }
    return it->second;
}

Client const& WindowManager::require_client(xcb_window_t window) const
{
    auto it = clients_.find(window);
    if (it == clients_.end())
    {
        LOG_CRITICAL("require_client: window {:#x} not in clients registry", window);
        std::abort();
    }
    return it->second;
}

// Preserve 0xFFFFFFFF (sticky); absence or an unreadable property yields nullopt.
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

WindowManager::DesktopResolutionResult WindowManager::resolve_window_desktop(xcb_window_t window) const
{
    if (config_.workspaces.count == 0)
        return { DesktopResolution::NoHint };

    auto desktop = get_window_desktop(window);
    if (!desktop)
        return { DesktopResolution::NoHint };

    auto indices = ewmh_policy::desktop_to_indices(*desktop, config_.workspaces.count);
    if (!indices)
        return { DesktopResolution::OutOfRange };
    size_t monitor_idx = indices->first;
    size_t workspace_idx = indices->second;

    if (monitor_idx >= monitors_.size() || workspace_idx >= monitors_[monitor_idx].workspaces.size())
        return { DesktopResolution::OutOfRange };

    return { DesktopResolution::Resolved, monitor_idx, workspace_idx };
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

bool WindowManager::should_be_visible(Client const& client) const
{
    return visibility_policy::is_window_visible(
        showing_desktop_,
        client.iconic,
        client.sticky,
        client.monitor,
        client.workspace,
        monitors_
    );
}

bool WindowManager::is_physically_visible(Client const& client) const
{
    return !client.hidden && should_be_visible(client);
}

xcb_window_t WindowManager::select_fullscreen_owner_for_monitor(size_t monitor_idx, xcb_window_t preferred_owner) const
{
    if (monitor_idx >= monitors_.size() || showing_desktop_)
        return XCB_NONE;

    std::vector<fullscreen_policy::FullscreenCandidate> candidates;
    candidates.reserve(clients_.size());
    for (auto const& [id, client] : clients_)
    {
        if (client.monitor != monitor_idx)
            continue;
        if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
            continue;
        candidates.push_back({
            id,
            client.fullscreen && !client.iconic && should_be_visible(client),
            client.order,
        });
    }

    if (preferred_owner != XCB_NONE)
    {
        for (auto const& candidate : candidates)
        {
            if (candidate.window == preferred_owner && candidate.eligible)
                return preferred_owner;
        }
    }

    return fullscreen_policy::select_owner(monitors_[monitor_idx].fullscreen_owner, candidates);
}

bool WindowManager::is_suppressed_by_fullscreen(Client const& client) const
{
    if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
        return false;
    if (client.monitor >= monitors_.size())
        return false;
    if (client.iconic || !should_be_visible(client))
        return false;

    xcb_window_t const owner = monitors_[client.monitor].fullscreen_owner;
    if (owner == XCB_NONE || owner == client.id)
        return false;
    if (client.transient_for == owner)
        return false;
    return true;
}

stacking_policy::Tier WindowManager::compute_stack_tier(Client const& client) const
{
    if (client.kind() == Client::Kind::Desktop)
        return stacking_policy::Tier::Below;
    if (client.kind() == Client::Kind::Dock)
        return stacking_policy::Tier::Above;

    return stacking_policy::compute_tier(
        is_suppressed_by_fullscreen(client),
        client.fullscreen,
        client.layer_hint == LayerHint::Above,
        client.layer_hint == LayerHint::Below,
        client.modal
    );
}

stacking_policy::ClientStackInputs WindowManager::stack_inputs_of(Client const& client) const
{
    bool policy_visible = client.kind() == Client::Kind::Tiled || client.kind() == Client::Kind::Floating
        ? should_be_visible(client)
        : true;
    return stacking_policy::ClientStackInputs{
        client.id,
        policy_visible,
        compute_stack_tier(client),
        client.kind() == Client::Kind::Floating,
        client.id == active_window_,
        client.order,
    };
}

void WindowManager::apply_stacking()
{
    stacking_dirty_ = false;

    std::vector<stacking_policy::ClientStackInputs> inputs;
    inputs.reserve(clients_.size());
    for (auto const& [window, client] : clients_) inputs.push_back(stack_inputs_of(client));

    auto order = stacking_policy::compute_order(inputs);

    auto position_of = [&order](xcb_window_t w) -> std::optional<size_t>
    {
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == w)
                return i;
        return std::nullopt;
    };

    auto transient_can_stack = [this](Client const& client)
    {
        if (client.kind() != Client::Kind::Floating || client.transient_for == XCB_NONE)
            return false;
        if (!is_physically_visible(client) || is_suppressed_by_fullscreen(client))
            return false;
        auto const* parent = get_client(client.transient_for);
        return parent && is_physically_visible(*parent) && !is_suppressed_by_fullscreen(*parent);
    };

    for (size_t pass = 0; pass < order.size(); ++pass)
    {
        bool changed = false;
        auto snapshot = order;
        for (xcb_window_t window : snapshot)
        {
            auto const* client = get_client(window);
            if (!client || !transient_can_stack(*client))
                continue;

            auto child_pos = position_of(window);
            auto parent_pos = position_of(client->transient_for);
            if (!child_pos || !parent_pos || *child_pos > *parent_pos)
                continue;

            xcb_window_t child = order[*child_pos];
            order.erase(order.begin() + static_cast<std::ptrdiff_t>(*child_pos));
            parent_pos = position_of(client->transient_for);
            if (!parent_pos)
                continue;
            order.insert(order.begin() + static_cast<std::ptrdiff_t>(*parent_pos + 1), child);
            changed = true;
        }
        if (!changed)
            break;
    }

    std::vector<xcb_window_t> visible;
    visible.reserve(order.size());
    for (auto window : order)
        if (auto const* client = get_client(window); client && stack_inputs_of(*client).visible)
            visible.push_back(window);

    if (visible.size() > 1)
    {
        // Read server truth instead of trusting a cached desired order: external
        // restacks must still be repaired, including when our policy is unchanged.
        auto cookie = xcb_query_tree(conn_.get(), conn_.screen()->root);
        auto* reply = xcb_query_tree_reply(conn_.get(), cookie, nullptr);
        std::vector<stacking_policy::StackMove> moves;
        if (reply)
            moves = stacking_policy::plan_moves(
                { xcb_query_tree_children(reply), static_cast<size_t>(xcb_query_tree_children_length(reply)) },
                visible
            );
        else
            for (size_t i = 1; i < visible.size(); ++i)
                moves.push_back({ visible[i], visible[i - 1], XCB_STACK_MODE_ABOVE });
        free(reply);
        for (auto const& move : moves)
        {
            uint32_t values[] = { move.sibling, move.mode };
            xcb_configure_window(
                conn_.get(),
                move.window,
                XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
                values
            );
        }
    }

    ewmh_.update_client_list_stacking(order);
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
}

// Notification only: advance the sequence before configure without waiting on
// the client counter. See X11.md for the supported sync-request behavior.
void WindowManager::send_sync_request(Client& client, uint32_t timestamp)
{
    if (wm_protocols_ == XCB_NONE || net_wm_sync_request_ == XCB_NONE)
        return;

    if (client.sync_counter == 0)
        return;

    uint64_t value = ++client.sync_value;

    // _NET_WM_SYNC_REQUEST is sent via WM_PROTOCOLS (EWMH spec)
    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = client.id;
    ev.type = wm_protocols_;
    ev.format = 32;
    ev.data.data32[0] = net_wm_sync_request_;
    ev.data.data32[1] = timestamp ? timestamp : XCB_CURRENT_TIME;
    ev.data.data32[2] = static_cast<uint32_t>(value & 0xffffffff);
    ev.data.data32[3] = static_cast<uint32_t>((value >> 32) & 0xffffffff);

    xcb_send_event(conn_.get(), 0, client.id, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
    // Non-blocking: we don't wait for the client to update its counter
}

void WindowManager::update_sync_state(Client& client)
{
    if (net_wm_sync_request_counter_ == XCB_NONE || net_wm_sync_request_ == XCB_NONE)
        return;

    if (!supports_protocol(client.id, net_wm_sync_request_))
    {
        client.sync_counter = 0;
        client.sync_value = 0;
        return;
    }

    auto cookie = xcb_get_property(conn_.get(), 0, client.id, net_wm_sync_request_counter_, XCB_ATOM_CARDINAL, 0, 1);
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
        client.sync_counter = 0;
        client.sync_value = 0;
        return;
    }

    client.sync_counter = counter;

    auto counter_cookie = xcb_sync_query_counter(conn_.get(), counter);
    auto* counter_reply = xcb_sync_query_counter_reply(conn_.get(), counter_cookie, nullptr);
    if (counter_reply)
    {
        uint64_t value = (static_cast<uint64_t>(counter_reply->counter_value.hi) << 32)
            | static_cast<uint64_t>(counter_reply->counter_value.lo);
        client.sync_value = value;
        free(counter_reply);
    }
    else
    {
        client.sync_value = 0;
    }
}

void WindowManager::update_fullscreen_monitor_state(Client& client)
{
    if (net_wm_fullscreen_monitors_ == XCB_NONE)
        return;

    xcb_ewmh_get_wm_fullscreen_monitors_reply_t reply;
    if (!xcb_ewmh_get_wm_fullscreen_monitors_reply(
            ewmh_.get(),
            xcb_ewmh_get_wm_fullscreen_monitors(ewmh_.get(), client.id),
            &reply,
            nullptr
        ))
    {
        client.fullscreen_monitors.reset();
        return;
    }

    FullscreenMonitors monitors;
    monitors.top = reply.top;
    monitors.bottom = reply.bottom;
    monitors.left = reply.left;
    monitors.right = reply.right;
    client.fullscreen_monitors = monitors;
}

void WindowManager::apply_geometry(Client& client, Geometry geometry, uint32_t border_width)
{
    geometry.width = std::max<uint16_t>(1, geometry.width);
    geometry.height = std::max<uint16_t>(1, geometry.height);
    send_sync_request(client, last_event_time_);
    uint32_t values[] = { static_cast<uint32_t>(geometry.x),
                          static_cast<uint32_t>(geometry.y),
                          geometry.width,
                          geometry.height,
                          border_width };
    xcb_configure_window(
        conn_.get(),
        client.id,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
            | XCB_CONFIG_WINDOW_BORDER_WIDTH,
        values
    );
    send_configure_notify(client.id, geometry, static_cast<uint16_t>(border_width));
}

void WindowManager::send_configure_notify(xcb_window_t window, Geometry const& geom, uint16_t border_width)
{
    xcb_configure_notify_event_t ev = {};
    ev.response_type = XCB_CONFIGURE_NOTIFY;
    ev.event = window;
    ev.window = window;
    ev.x = geom.x;
    ev.y = geom.y;
    ev.width = geom.width;
    ev.height = geom.height;
    ev.border_width = border_width;
    ev.above_sibling = XCB_NONE;
    ev.override_redirect = 0;

    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<char*>(&ev));
}

void WindowManager::send_configure_notify(Client const& client)
{
    Geometry geom{};
    if (client.kind() == Client::Kind::Tiled)
        geom = client.tiled_geometry;
    else if (client.fullscreen)
        geom = fullscreen_geometry_for_client(client);
    else if (client.kind() == Client::Kind::Floating)
        geom = floating_geometry(client);

    // Fall through to X read if cached geometry is uninitialized (e.g. iconic tiled never laid out)
    if (geom.width > 0 && geom.height > 0)
    {
        send_configure_notify(client.id, geom, static_cast<uint16_t>(border_width_for_client(client)));
        return;
    }

    auto geom_cookie = xcb_get_geometry(conn_.get(), client.id);
    auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
    if (!geom_reply)
        return;

    Geometry fallback_geom = { geom_reply->x, geom_reply->y, geom_reply->width, geom_reply->height };
    uint16_t bw = geom_reply->border_width;
    free(geom_reply);
    send_configure_notify(client.id, fallback_geom, bw);
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
    if (!result.monitor_changed())
        return;

    // We crossed monitors - update active monitor and clear focus
    focused_monitor_ = result.new_monitor;
    update_ewmh_current_desktop();
    if (result.clears_focus())
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
    auto* client = get_client(window);
    if (!client)
        return;
    auto name = get_window_name(window);
    if (name == client->name)
        return;
    auto previous = window_rules_.match(window_match_info(*client), monitors_, config_.workspaces.names);
    client->name = std::move(name);
    reevaluate_metadata(window, previous);
}

void WindowManager::update_struts()
{
    for (auto& monitor : monitors_)
    {
        monitor.strut = {};
    }

    for (auto const& [dock, client] : clients_)
    {
        if (client.kind() != Client::Kind::Dock)
            continue;

        Strut strut = ewmh_.get_window_strut(dock);
        if (strut.left == 0 && strut.right == 0 && strut.top == 0 && strut.bottom == 0)
            continue;

        auto geom_cookie = xcb_get_geometry(conn_.get(), dock);
        auto* geom = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
        if (!geom)
            continue;

        Monitor* target = monitor_at_point(geom->x, geom->y);
        free(geom);

        if (target)
        {
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
    auto it = clients_.find(window);
    if (it != clients_.end() && it->second.kind() == Client::Kind::Dock)
    {
        if (wm_state_ != XCB_NONE)
        {
            uint32_t data[] = { WM_STATE_WITHDRAWN, 0 };
            xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
        }
        clients_.erase(it);
        update_struts();
        rearrange_all_monitors();
        update_ewmh_client_list();
    }
}

void WindowManager::unmanage_desktop_window(xcb_window_t window)
{
    auto it = clients_.find(window);
    if (it != clients_.end() && it->second.kind() == Client::Kind::Desktop)
    {
        if (wm_state_ != XCB_NONE)
        {
            uint32_t data[] = { WM_STATE_WITHDRAWN, 0 };
            xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
        }
        clients_.erase(it);
        update_ewmh_client_list();
    }
}

void WindowManager::assign_window_workspace(Client& client, size_t monitor_idx, size_t workspace_idx)
{
    if (monitor_idx >= monitors_.size() || workspace_idx >= monitors_[monitor_idx].workspaces.size())
    {
        LOG_WARN("assign_window_workspace: invalid indices monitor={} workspace={}", monitor_idx, workspace_idx);
        return;
    }

    client.monitor = monitor_idx;
    client.workspace = workspace_idx;
    if (client.sticky)
        ewmh_.set_window_desktop(client.id, 0xFFFFFFFF);
    else
        ewmh_.set_window_desktop(client.id, get_ewmh_desktop_index(monitor_idx, workspace_idx));
}

bool WindowManager::move_tiled_client_to_workspace(
    Client& client,
    size_t target_monitor,
    size_t target_workspace,
    std::optional<size_t> insert_index
)
{
    if (client.kind() != Client::Kind::Tiled)
        return false;
    if (target_monitor >= monitors_.size() || target_workspace >= monitors_[target_monitor].workspaces.size())
        return false;

    xcb_window_t window = client.id;
    size_t source_monitor = client.monitor;
    size_t source_workspace = client.workspace;
    if (source_monitor == target_monitor && source_workspace == target_workspace && !insert_index.has_value())
        return true;

    bool same_workspace = source_monitor == target_monitor && source_workspace == target_workspace;
    auto& source_ws = monitors_[source_monitor].workspaces[source_workspace];
    auto source_it = source_ws.find_window(window);
    if (source_it == source_ws.windows.end())
        return false;

    source_ws.windows.erase(source_it);
    workspace_policy::remove_from_focus_history(source_ws, window);
    if (!same_workspace)
    {
        workspace_policy::fixup_workspace_focus(
            source_ws,
            window,
            [this](xcb_window_t w) { return window_is_iconic(w); }
        );
    }

    assign_window_workspace(client, target_monitor, target_workspace);

    auto& target_ws = monitors_[target_monitor].workspaces[target_workspace];
    size_t target_index = insert_index.value_or(target_ws.windows.size());
    target_index = std::min(target_index, target_ws.windows.size());
    target_ws.windows.insert(target_ws.windows.begin() + static_cast<std::ptrdiff_t>(target_index), window);

    finalize_move_visibility(source_monitor, target_monitor);

    return true;
}

bool WindowManager::move_floating_client_to_workspace(
    Client& client,
    size_t target_monitor,
    size_t target_workspace,
    bool place_on_monitor_change
)
{
    if (client.kind() != Client::Kind::Floating)
        return false;
    if (target_monitor >= monitors_.size() || target_workspace >= monitors_[target_monitor].workspaces.size())
        return false;

    size_t source_monitor = client.monitor;
    bool monitor_changed = source_monitor != target_monitor;
    if (!monitor_changed && client.workspace == target_workspace)
        return true;

    if (monitor_changed && place_on_monitor_change)
    {
        auto& geom = floating_geometry(client);
        geom =
            floating::place_floating(monitors_[target_monitor].working_area(), geom.width, geom.height, std::nullopt);
    }

    assign_window_workspace(client, target_monitor, target_workspace);

    finalize_move_visibility(source_monitor, target_monitor);
    apply_visible_floating_geometry(client);

    return true;
}

void WindowManager::add_tiled_to_workspace(Client& client, size_t monitor_idx, size_t workspace_idx)
{
    if (monitor_idx >= monitors_.size() || workspace_idx >= monitors_[monitor_idx].workspaces.size())
    {
        LOG_WARN("add_tiled_to_workspace: invalid indices monitor={} workspace={}", monitor_idx, workspace_idx);
        return;
    }
    monitors_[monitor_idx].workspaces[workspace_idx].windows.push_back(client.id);
    assign_window_workspace(client, monitor_idx, workspace_idx);
}

void WindowManager::remove_tiled_from_workspace(Client const& client, size_t monitor_idx, size_t workspace_idx)
{
    if (monitor_idx >= monitors_.size() || workspace_idx >= monitors_[monitor_idx].workspaces.size())
    {
        LOG_WARN("remove_tiled_from_workspace: invalid indices monitor={} workspace={}", monitor_idx, workspace_idx);
        return;
    }
    workspace_policy::remove_tiled_window(
        monitors_[monitor_idx].workspaces[workspace_idx],
        client.id,
        [this](xcb_window_t w) { return window_is_iconic(w); }
    );
}

// Keep clients mapped while hidden; UnmapNotify is reserved for withdrawal.
void WindowManager::hide_window(Client& client)
{
    xcb_window_t window = client.id;
    LOG_TRACE("hide_window({:#x}) called", window);

    if (client.hidden)
    {
        LOG_TRACE("hide_window({:#x}): already hidden, skipping", window);
        return;
    }

    client.hidden = true;

    // Move window off-screen (preserve y coordinate for restore)
    uint32_t values[] = { static_cast<uint32_t>(OFF_SCREEN_X) };
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_X, values);

    LOG_TRACE("hide_window({:#x}): moved to x={}", window, OFF_SCREEN_X);
}

// Clear the flag only. The caller must restore geometry through layout or
// apply_floating_geometry before completing the visibility transition.
void WindowManager::show_window(Client& client)
{
    xcb_window_t window = client.id;
    LOG_TRACE("show_window({:#x}) called", window);

    if (!client.hidden)
    {
        LOG_TRACE("show_window({:#x}): not hidden, skipping", window);
        return;
    }

    client.hidden = false;
    LOG_TRACE("show_window({:#x}): marked as visible", window);
}

void WindowManager::flush_and_drain_crossing()
{
    flush_stacking_list();
    conn_.flush();

    // Round-trip sync: ensures the server has processed all our requests
    // and generated all resulting events into the connection buffer.
    auto cookie = xcb_get_input_focus(conn_.get());
    free(xcb_get_input_focus_reply(conn_.get(), cookie, nullptr));

    // Drain crossing events generated by our visibility changes (moving
    // windows on/off-screen).  Without this, the next event-loop iteration
    // would see EnterNotify/MotionNotify for whichever window now sits under
    // the cursor, overriding the focus we just set via focus_or_fallback().
    // Other events are deferred to the top-level event loop so transition
    // helpers do not re-enter arbitrary handlers while holding Client refs.
    while (auto* event = xcb_poll_for_event(conn_.get()))
    {
        uint8_t type = event->response_type & ~0x80;
        if (type == XCB_ENTER_NOTIFY || type == XCB_LEAVE_NOTIFY || type == XCB_MOTION_NOTIFY)
        {
            free(event);
            continue;
        }
        deferred_events_.push_back(*event);
        free(event);
    }
}

void WindowManager::cache_focus_hints_into(Client& client)
{
    xcb_window_t window = client.id;
    // Read WM_HINTS directly from X (the cached default would be stale).
    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), window), &hints, nullptr))
        client.accepts_input = !(hints.flags & XCB_ICCCM_WM_HINT_INPUT) || hints.input;
    else
        client.accepts_input = true; // ICCCM default when WM_HINTS absent

    client.supports_take_focus = supports_protocol(window, wm_take_focus_);
}

void WindowManager::cache_focus_hints(xcb_window_t window)
{
    if (auto* client = get_client(window))
        cache_focus_hints_into(*client);
}

void WindowManager::finalize_move_visibility(size_t source_monitor, size_t target_monitor)
{
    if (source_monitor >= monitors_.size() || target_monitor >= monitors_.size())
    {
        LOG_WARN(
            "finalize_move_visibility: invalid source={} target={} monitor_count={}",
            source_monitor,
            target_monitor,
            monitors_.size()
        );
        return;
    }

    reconcile_visibility_for_monitor(source_monitor);
    if (target_monitor != source_monitor)
        reconcile_visibility_for_monitor(target_monitor);

    rearrange_monitor(monitors_[source_monitor]);
    if (target_monitor != source_monitor)
        rearrange_monitor(monitors_[target_monitor]);
}

void WindowManager::finalize_removed_window_after_unmanage(
    size_t monitor_idx,
    bool removed_active_window,
    bool focus_fallback_if_active
)
{
    if (monitor_idx < monitors_.size())
        finalize_visibility_on_monitor(monitor_idx);

    if (removed_active_window)
    {
        if (focus_fallback_if_active && monitor_idx == focused_monitor_ && monitor_idx < monitors_.size())
            focus_or_fallback(monitors_[monitor_idx]);
        else
            clear_focus();
    }

    flush_and_drain_crossing();
}

void WindowManager::reconcile_visibility_for_monitor(size_t monitor_idx, xcb_window_t preferred_owner)
{
    if (monitor_idx >= monitors_.size())
    {
        LOG_WARN("reconcile_visibility_for_monitor: invalid monitor_idx {}", monitor_idx);
        return;
    }

    monitors_[monitor_idx].fullscreen_owner = select_fullscreen_owner_for_monitor(monitor_idx, preferred_owner);

    for (auto& [id, client] : clients_)
    {
        (void)id;
        if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
            continue;
        if (client.monitor != monitor_idx)
            continue;

        bool const should_show = should_be_visible(client) && !is_suppressed_by_fullscreen(client);

        if (should_show && client.hidden)
        {
            show_window(client);
            if (client.kind() == Client::Kind::Floating)
            {
                if (client.fullscreen)
                    apply_fullscreen_if_needed(client);
                else if (client.maximized_horz || client.maximized_vert)
                    apply_maximized_geometry(client);
                else
                    apply_floating_geometry(client);
            }
        }
        else if (!should_show && !client.hidden)
        {
            hide_window(client);
        }
    }

    stacking_dirty_ = true;
}

void WindowManager::finalize_visibility_on_monitor(size_t monitor_idx, xcb_window_t preferred_fullscreen_owner)
{
    if (monitor_idx >= monitors_.size())
    {
        LOG_WARN("finalize_visibility_on_monitor: invalid monitor_idx {}", monitor_idx);
        return;
    }

    reconcile_visibility_for_monitor(monitor_idx, preferred_fullscreen_owner);
    rearrange_monitor(monitors_[monitor_idx]);
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
    for (auto const& [window, client] : clients_)
    {
        if (client.kind() == Client::Kind::Floating)
            xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &conn_.screen()->black_pixel);
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
