#include "wm.hpp"
#include "lwm/core/floating.hpp"
#include "lwm/core/focus.hpp"
#include "lwm/core/ipc.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <poll.h>
#include <string_view>
#include <tuple>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb_icccm.h>

namespace lwm {

namespace {

constexpr auto KILL_TIMEOUT = std::chrono::seconds(5);

constexpr size_t IPC_MAX_REQUEST_SIZE = 4096;
constexpr auto IPC_CLIENT_TIMEOUT = std::chrono::milliseconds(500);

enum class StackLayer
{
    Below = 0,
    Normal = 1,
    Above = 2,
    Fullscreen = 3,
    Overlay = 4
};

std::string trim_ascii(std::string_view value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
        ++start;

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;

    return std::string(value.substr(start, end - start));
}

std::string ok_reply(std::string const& message)
{
    if (message.empty())
        return "ok";
    return "ok " + message;
}

std::string error_reply(std::string const& message) { return "error " + message; }

bool is_notification_attention_param_start(std::string_view params, size_t pos)
{
    if (pos >= params.size() || !std::isspace(static_cast<unsigned char>(params[pos])))
        return false;

    size_t key_start = pos + 1;
    constexpr std::string_view keys[] = { "window", "desktop-entry", "app-name" };
    for (std::string_view key : keys)
    {
        if (params.substr(key_start).starts_with(key))
        {
            size_t after_key = key_start + key.size();
            if (after_key < params.size() && params[after_key] == '=')
                return true;
        }
    }
    return false;
}

size_t find_next_notification_attention_param(std::string_view params)
{
    for (size_t i = 0; i < params.size(); ++i)
    {
        if (is_notification_attention_param_start(params, i))
            return i;
    }
    return std::string_view::npos;
}

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
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
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
    , layout_(conn_, config_.appearance, config_.layout)
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

    // Create cursors for tiled resize hover feedback
    {
        xcb_font_t font = xcb_generate_id(conn_.get());
        xcb_open_font(conn_.get(), font, 6, "cursor");

        cursor_default_ = xcb_generate_id(conn_.get());
        xcb_create_glyph_cursor(conn_.get(), cursor_default_, font, font,
            68, 69, 0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF); // left_ptr

        cursor_resize_h_ = xcb_generate_id(conn_.get());
        xcb_create_glyph_cursor(conn_.get(), cursor_resize_h_, font, font,
            108, 109, 0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF); // sb_h_double_arrow

        cursor_resize_v_ = xcb_generate_id(conn_.get());
        xcb_create_glyph_cursor(conn_.get(), cursor_resize_v_, font, font,
            116, 117, 0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF); // sb_v_double_arrow

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
        clean_restart_properties();
        // Restore focus to the previously active window
        if (active_window_ != XCB_NONE && is_managed(active_window_))
        {
            focus_any_window(active_window_);
        }
        else if (!monitors_.empty())
        {
            focus_or_fallback(monitors_[focused_monitor_]);
        }
    }
    else
    {
        run_autostart();
    }
    keybinds_.grab_keys(conn_.screen()->root);
    update_ewmh_client_list();
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
    int xfd = xcb_get_file_descriptor(conn_.get());

    // Poll fd index constants for fixed entries
    constexpr size_t POLL_X = 0;
    constexpr size_t POLL_SIGNAL = 1;
    constexpr size_t POLL_IPC_LISTENER = 2;
    constexpr size_t POLL_IPC_CLIENT = 3;
    constexpr size_t POLL_SUBSCRIBERS = 4;

    std::vector<pollfd> poll_fds;

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

        if (pending_ipc_)
        {
            if (!next_deadline || pending_ipc_->deadline < *next_deadline)
                next_deadline = pending_ipc_->deadline;
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

        // Build poll array: X fd, signal pipe, IPC listener, pending client, subscribers
        poll_fds.clear();
        poll_fds.push_back({.fd = xfd, .events = POLLIN, .revents = 0});
        poll_fds.push_back({.fd = signal_pipe_[0], .events = POLLIN, .revents = 0});
        poll_fds.push_back({.fd = ipc_listener_fd_, .events = POLLIN, .revents = 0});
        poll_fds.push_back({.fd = pending_ipc_ ? pending_ipc_->fd : -1, .events = POLLIN, .revents = 0});
        size_t polled_subscriber_count = subscribers_.size();
        for (auto const& sub : subscribers_)
            poll_fds.push_back({.fd = sub.fd, .events = 0, .revents = 0}); // only detect HUP/ERR

        int poll_result = poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), timeout_ms);
        if (poll_result > 0)
        {
            // SIGHUP: drain pipe and reload config
            if (poll_fds[POLL_SIGNAL].revents & POLLIN)
            {
                char buf[64];
                while (read(signal_pipe_[0], buf, sizeof(buf)) > 0) {}
                LOG_INFO("SIGHUP received, reloading config");
                auto result = reload_config();
                if (result)
                    LOG_INFO("Config reloaded successfully");
                else
                    LOG_ERROR("Config reload failed: {}", result.error());
                emit_config_reload_result(result, "sighup");
            }

            // Accept a new IPC client (only if no pending client)
            if (poll_fds[POLL_IPC_LISTENER].revents & POLLIN)
            {
                accept_ipc_client();
            }

            // Process readable data from pending IPC client
            if (pending_ipc_ && (poll_fds[POLL_IPC_CLIENT].revents & POLLIN))
            {
                process_ipc_client();
            }

            // Check subscriber disconnects (only those that were in the poll array)
            for (size_t i = 0; i < polled_subscriber_count; ++i)
            {
                if (poll_fds[POLL_SUBSCRIBERS + i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    close_fd(subscribers_[i].fd);
            }
            cleanup_dead_subscribers();

            while (auto event = xcb_poll_for_event(conn_.get()))
            {
                std::unique_ptr<xcb_generic_event_t, decltype(&free)> eventPtr(event, free);
                handle_event(*eventPtr);
            }
        }

        // Check IPC client deadline
        if (pending_ipc_ && std::chrono::steady_clock::now() >= pending_ipc_->deadline)
        {
            LOG_WARN("IPC client timed out");
            close_ipc_client();
        }

        handle_timeouts();
        flush_stacking_list();

        if (xcb_connection_has_error(conn_.get()))
        {
            LOG_ERROR("X connection error, shutting down");
            break;
        }
    }

    if (restarting_)
        return RunResult::Restart;
    return RunResult::Exit;
}

void WindowManager::setup_ipc()
{
    namespace fs = std::filesystem;

    fs::path socket_path = ipc::default_socket_path();
    fs::create_directories(socket_path.parent_path());

    ipc_socket_path_ = socket_path.string();
    if (ipc_socket_path_.size() >= sizeof(sockaddr_un::sun_path))
        throw std::runtime_error("IPC socket path is too long: " + ipc_socket_path_);

    unlink(ipc_socket_path_.c_str());

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        throw std::runtime_error("Failed to create IPC socket");

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, ipc_socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    socklen_t addr_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + ipc_socket_path_.size() + 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), addr_len) < 0)
    {
        int saved_errno = errno;
        close(fd);
        throw std::runtime_error("Failed to bind IPC socket '" + ipc_socket_path_ + "': " + std::strerror(saved_errno));
    }

    if (listen(fd, 16) < 0)
    {
        int saved_errno = errno;
        close(fd);
        unlink(ipc_socket_path_.c_str());
        throw std::runtime_error("Failed to listen on IPC socket '" + ipc_socket_path_ + "': " + std::strerror(saved_errno));
    }

    ipc_listener_fd_ = fd;
    chmod(ipc_socket_path_.c_str(), 0600);

    if (lwm_ipc_socket_ != XCB_NONE)
    {
        ipc::set_root_text_property(
            conn_.get(),
            conn_.screen()->root,
            lwm_ipc_socket_,
            utf8_string_,
            ipc_socket_path_
        );
        conn_.flush();
    }
}

void WindowManager::cleanup_ipc()
{
    close_ipc_client();
    for (auto& sub : subscribers_)
        close_fd(sub.fd);
    subscribers_.clear();
    close_fd(ipc_listener_fd_);

    if (!ipc_socket_path_.empty())
        unlink(ipc_socket_path_.c_str());

    if (lwm_ipc_socket_ != XCB_NONE && conn_.get() && !xcb_connection_has_error(conn_.get()))
    {
        ipc::delete_root_property(conn_.get(), conn_.screen()->root, lwm_ipc_socket_);
        conn_.flush();
    }
}

void WindowManager::emit_event(EventType type, std::string_view json)
{
    if (subscribers_.empty())
        return;

    // Check if any subscriber wants this event type before copying the string
    bool any_match = false;
    for (auto const& sub : subscribers_)
    {
        if (sub.fd >= 0 && (sub.event_mask & type))
        {
            any_match = true;
            break;
        }
    }
    if (!any_match)
        return;

    std::string line;
    line.reserve(json.size() + 1);
    line.append(json);
    line.push_back('\n');

    for (auto& sub : subscribers_)
    {
        if (sub.fd < 0 || !(sub.event_mask & type))
            continue;
        ssize_t sent = send(sub.fd, line.data(), line.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                close_fd(sub.fd);
        }
        else if (static_cast<size_t>(sent) < line.size())
        {
            // Partial write: stream is now corrupt, disconnect subscriber
            close_fd(sub.fd);
        }
    }
}

void WindowManager::cleanup_dead_subscribers()
{
    std::erase_if(subscribers_, [](Subscriber const& s) { return s.fd < 0; });
}

void WindowManager::accept_ipc_client()
{
    int client_fd = accept4(ipc_listener_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            LOG_WARN("IPC accept failed: {}", std::strerror(errno));
        return;
    }

    // If we already have a pending client, reject the new one
    if (pending_ipc_)
    {
        std::string reply = error_reply("busy") + '\n';
        send(client_fd, reply.data(), reply.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        close(client_fd);
        return;
    }

    pending_ipc_ = IpcClient{
        .fd = client_fd,
        .buffer = {},
        .deadline = std::chrono::steady_clock::now() + IPC_CLIENT_TIMEOUT,
    };
    pending_ipc_->buffer.reserve(128);
}

void WindowManager::process_ipc_client()
{
    if (!pending_ipc_)
        return;

    char buf[512];
    while (true)
    {
        ssize_t received = recv(pending_ipc_->fd, buf, sizeof(buf), 0);
        if (received > 0)
        {
            pending_ipc_->buffer.append(buf, static_cast<size_t>(received));

            if (pending_ipc_->buffer.size() >= IPC_MAX_REQUEST_SIZE)
            {
                std::string reply = error_reply("request too large") + '\n';
                send(pending_ipc_->fd, reply.data(), reply.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
                close_ipc_client();
                return;
            }

            if (pending_ipc_->buffer.find('\n') != std::string::npos)
            {
                // Complete message received - process it
                std::string request = pending_ipc_->buffer;
                size_t line_end = request.find('\n');
                if (line_end != std::string::npos)
                    request.resize(line_end);

                auto reply = run_ipc_command(request);
                if (reply)
                {
                    reply->push_back('\n');
                    send(pending_ipc_->fd, reply->data(), reply->size(), MSG_NOSIGNAL | MSG_DONTWAIT);
                    close_ipc_client();
                }
                return;
            }
            continue;
        }
        if (received == 0)
        {
            // Client closed connection - process whatever we have
            if (!pending_ipc_->buffer.empty())
            {
                auto reply = run_ipc_command(pending_ipc_->buffer);
                if (reply)
                {
                    reply->push_back('\n');
                    send(pending_ipc_->fd, reply->data(), reply->size(), MSG_NOSIGNAL | MSG_DONTWAIT);
                }
            }
            if (pending_ipc_)
                close_ipc_client();
            return;
        }
        // received < 0
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return; // No more data available right now, wait for next poll
        // Actual error
        LOG_WARN("IPC recv failed: {}", std::strerror(errno));
        close_ipc_client();
        return;
    }
}

void WindowManager::close_ipc_client()
{
    if (!pending_ipc_)
        return;
    close(pending_ipc_->fd);
    pending_ipc_.reset();
}

std::optional<std::string> WindowManager::run_ipc_command(std::string const& command)
{
    std::string trimmed = trim_ascii(command);
    if (trimmed.empty())
        return error_reply("empty command");

    if (trimmed == "ping")
        return ok_reply("pong");

    if (trimmed == "version")
        return ok_reply(LWM_VERSION);

    if (trimmed == "reload-config")
    {
        auto result = reload_config();
        emit_config_reload_result(result, "ipc");
        if (!result)
            return error_reply(result.error());
        return ok_reply("reloaded");
    }

    if (trimmed == "subscribe" || trimmed.starts_with("subscribe "))
    {
        if (subscribers_.size() >= MAX_SUBSCRIBERS)
            return error_reply("max subscribers reached");
        if (!pending_ipc_)
            return error_reply("no connection to promote");

        std::string filter_str;
        if (trimmed.starts_with("subscribe "))
            filter_str = trim_ascii(trimmed.substr(10));
        uint32_t mask = parse_event_filter(filter_str);
        if (mask == 0)
            return error_reply("no recognized event types in filter");

        // Send reply directly, then promote to subscriber
        std::string reply = ok_reply("subscribed") + '\n';
        send(pending_ipc_->fd, reply.data(), reply.size(), MSG_NOSIGNAL | MSG_DONTWAIT);

        subscribers_.push_back({.fd = pending_ipc_->fd, .event_mask = mask});
        pending_ipc_->fd = -1; // prevent close_ipc_client from closing it
        pending_ipc_.reset();
        return std::nullopt; // connection promoted to subscriber
    }

    if (trimmed == "restart")
    {
        initiate_restart();
        return ok_reply("restarting");
    }

    if (trimmed.starts_with("exec "))
    {
        std::string binary = trim_ascii(trimmed.substr(5));
        if (binary.empty())
            return error_reply("exec requires a binary path");
        initiate_restart(std::move(binary));
        return ok_reply("restarting");
    }

    // Layout commands
    {
        constexpr std::string_view layout_set_prefix = "layout set ";
        if (trimmed.starts_with(layout_set_prefix))
        {
            std::string name = trim_ascii(trimmed.substr(layout_set_prefix.size()));
            if (name == "master-stack")
            {
                focused_monitor().current().layout_strategy = LayoutStrategy::MasterStack;
                rearrange_monitor(focused_monitor(), true);
                emit_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"layout_set\",\"value\":\"master-stack\"}");
                return ok_reply("layout set to master-stack");
            }
            return error_reply("unknown layout: " + name);
        }
    }

    {
        constexpr std::string_view ratio_set_prefix = "ratio set ";
        if (trimmed.starts_with(ratio_set_prefix))
        {
            std::string val_str = trim_ascii(trimmed.substr(ratio_set_prefix.size()));
            double val;
            try
            {
                val = std::stod(val_str);
            }
            catch (...)
            {
                return error_reply("invalid ratio value: " + val_str);
            }
            double min_r = config_.layout.min_ratio;
            if (val < min_r || val > 1.0 - min_r)
                return error_reply("ratio out of range [" + std::to_string(min_r) + ", " + std::to_string(1.0 - min_r) + "]");
            focused_monitor().current().split_ratios[SplitAddress { 0, 0 }] = val;
            rearrange_monitor(focused_monitor(), true);
            emit_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"ratio_set\",\"value\":" + std::to_string(val) + "}");
            return ok_reply("ratio set");
        }
    }

    if (trimmed == "ratio reset")
    {
        focused_monitor().current().split_ratios.clear();
        rearrange_monitor(focused_monitor(), true);
        emit_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"ratio_reset\"}");
        return ok_reply("ratios reset");
    }

    {
        constexpr std::string_view ratio_adj_prefix = "ratio adjust ";
        if (trimmed.starts_with(ratio_adj_prefix))
        {
            std::string delta_str = trim_ascii(trimmed.substr(ratio_adj_prefix.size()));
            double delta;
            try
            {
                delta = std::stod(delta_str);
            }
            catch (...)
            {
                return error_reply("invalid delta value: " + delta_str);
            }
            adjust_master_ratio(delta);
            emit_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"ratio_adjust\",\"delta\":" + std::to_string(delta) + "}");
            return ok_reply("ratio adjusted");
        }
    }

    if (trimmed == "notify-attention" || trimmed.starts_with("notify-attention "))
    {
        std::string_view params = trimmed;
        params.remove_prefix(std::string_view("notify-attention").size());
        while (!params.empty() && params.front() == ' ')
            params.remove_prefix(1);

        NotificationAttentionRequest req;
        while (!params.empty())
        {
            size_t eq = params.find('=');
            if (eq == std::string_view::npos)
                return error_reply("invalid parameter (expected key=value): " + std::string(params));

            std::string_view key = params.substr(0, eq);
            params.remove_prefix(eq + 1);

            size_t next_param = find_next_notification_attention_param(params);
            std::string value = trim_ascii(params.substr(0, next_param));
            params.remove_prefix(next_param == std::string_view::npos ? params.size() : next_param);
            while (!params.empty() && std::isspace(static_cast<unsigned char>(params.front())))
                params.remove_prefix(1);

            if (key == "window")
            {
                uint32_t xid = 0;
                int base = 10;
                std::string_view digits = value;
                if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
                {
                    base = 16;
                    digits.remove_prefix(2);
                }
                auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), xid, base);
                if (ec != std::errc {} || ptr != digits.data() + digits.size())
                    return error_reply("invalid window id: " + value);
                req.window = xid;
            }
            else if (key == "desktop-entry")
            {
                req.desktop_entry = std::move(value);
            }
            else if (key == "app-name")
            {
                req.app_name = std::move(value);
            }
            else
            {
                return error_reply("unknown parameter: " + std::string(key));
            }
        }

        if (req.window == XCB_NONE && req.desktop_entry.empty() && req.app_name.empty())
            return error_reply("notify-attention requires at least one of: window, desktop-entry, app-name");

        return handle_notification_attention(req);
    }

    if (trimmed == "scratchpad stash")
    {
        if (active_window_ != XCB_NONE)
            stash_to_scratchpad(active_window_);
        return ok_reply("");
    }
    if (trimmed == "scratchpad cycle")
    {
        cycle_scratchpad_pool();
        return ok_reply("");
    }
    if (trimmed.starts_with("scratchpad toggle "))
    {
        std::string name(trimmed.substr(18));
        toggle_named_scratchpad(name);
        return ok_reply("");
    }
    if (trimmed == "scratchpad list")
    {
        std::string json = "{\"named\":[";
        for (size_t i = 0; i < named_scratchpads_.size(); ++i)
        {
            if (i > 0)
                json += ",";
            auto const& sp = named_scratchpads_[i];
            json += "{\"name\":\"" + json_escape(sp.name) + "\",\"window\":" + std::to_string(sp.window) + ",\"pending\":" + (sp.pending_launch ? "true" : "false") + "}";
        }
        json += "],\"pool\":[";
        for (size_t i = 0; i < scratchpad_pool_.size(); ++i)
        {
            if (i > 0)
                json += ",";
            json += std::to_string(scratchpad_pool_[i]);
        }
        json += "]}";
        return json;
    }

    return error_reply("unknown command");
}

void WindowManager::emit_config_reload_result(std::expected<void, std::string> const& result, char const* source)
{
    if (result)
        emit_event(Event_ConfigReload,
            std::string("{\"event\":\"config_reload\",\"success\":true,\"source\":\"") + source + "\"}");
    else
        emit_event(Event_ConfigReload,
            std::string("{\"event\":\"config_reload\",\"success\":false,\"source\":\"") + source
            + "\",\"error\":\"" + json_escape(result.error()) + "\"}");
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
            NamedScratchpadState state { sp.name, XCB_NONE, false };
            // Preserve existing window claim if scratchpad name survived reload
            for (auto& old : old_states)
            {
                if (old.name == sp.name && old.window != XCB_NONE)
                {
                    state.window = old.window;
                    break;
                }
            }
            named_scratchpads_.push_back(std::move(state));
        }
        rebuild_scratchpad_matchers();

        // Unhide windows orphaned by removed scratchpad names
        for (auto& [window, client] : clients_)
        {
            if (!client.scratchpad_name.has_value())
                continue;
            if (!find_named_scratchpad(*client.scratchpad_name))
            {
                LOG_INFO("Scratchpad '{}' removed from config, restoring window {:#x}",
                    *client.scratchpad_name, window);
                client.scratchpad_name.reset();
                if (client.in_scratchpad && client.iconic)
                {
                    client.in_scratchpad = false;
                    client.iconic = false;
                    set_iconic_state(window, false);
                }
            }
        }
    }
    update_ewmh_desktops();
    reapply_rules_to_existing_windows();
    rearrange_all_monitors();

    if (!monitors_.empty())
    {
        if (auto* active = get_client(active_window_); active && active->monitor < monitors_.size()
            && is_focus_eligible(active_window_) && is_physically_visible(active_window_))
        {
            focused_monitor_ = active->monitor;
            if (active->kind == Client::Kind::Tiled && active->workspace < monitors_[active->monitor].workspaces.size())
            {
                workspace_policy::set_workspace_focus(
                    monitors_[active->monitor].workspaces[active->workspace], active_window_);
            }
            update_ewmh_current_desktop();
        }
        else
        {
            if (focused_monitor_ >= monitors_.size())
                focused_monitor_ = 0;
            focus_or_fallback(monitors_[focused_monitor_]);
        }
    }
    else
    {
        clear_focus();
    }

    apply_appearance_reload();
    conn_.flush();
    return {};
}

void WindowManager::regrab_all_keys()
{
    keybinds_.grab_keys(conn_.screen()->root);

    std::vector<std::pair<uint64_t, xcb_window_t>> ordered;
    ordered.reserve(clients_.size());
    for (auto const& [window, client] : clients_)
    {
        if (client.kind == Client::Kind::Tiled || client.kind == Client::Kind::Floating)
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
    if (client.fullscreen || client.borderless || client.layer == WindowLayer::Overlay)
        return 0U;
    return config_.appearance.border_width;
}

uint32_t WindowManager::border_color_for_client(Client const& client) const
{
    if (client.id == active_window_)
        return config_.appearance.border_color;
    if (client.demands_attention)
        return config_.appearance.urgent_border_color;
    return conn_.screen()->black_pixel;
}

bool WindowManager::should_apply_focus_border(xcb_window_t window) const
{
    auto const* client = get_client(window);
    if (!client)
        return false;
    if (!focus_policy::should_apply_focus_border(client->fullscreen))
        return false;
    return border_width_for_client(*client) > 0;
}

void WindowManager::apply_appearance_reload()
{
    for (auto const& [window, client] : clients_)
    {
        if (client.kind != Client::Kind::Tiled && client.kind != Client::Kind::Floating)
            continue;

        uint32_t border_width = border_width_for_client(client);
        uint32_t border_color = border_color_for_client(client);

        xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &border_color);
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);
    }
}

void WindowManager::update_allowed_actions(xcb_window_t window)
{
    auto const* client = get_client(window);
    if (!client)
        return;

    xcb_ewmh_connection_t* ewmh = ewmh_.get();
    std::vector<xcb_atom_t> actions = {
        ewmh->_NET_WM_ACTION_CLOSE,
        ewmh->_NET_WM_ACTION_CHANGE_DESKTOP,
        ewmh->_NET_WM_ACTION_MINIMIZE,
        ewmh->_NET_WM_ACTION_STICK,
    };

    if (client->layer != WindowLayer::Overlay)
    {
        actions.push_back(ewmh->_NET_WM_ACTION_FULLSCREEN);
        actions.push_back(ewmh->_NET_WM_ACTION_ABOVE);
        actions.push_back(ewmh->_NET_WM_ACTION_BELOW);
        actions.push_back(ewmh->_NET_WM_ACTION_MAXIMIZE_VERT);
        actions.push_back(ewmh->_NET_WM_ACTION_MAXIMIZE_HORZ);
    }

    if (client->kind == Client::Kind::Floating && client->layer != WindowLayer::Overlay)
    {
        actions.push_back(ewmh->_NET_WM_ACTION_MOVE);
        actions.push_back(ewmh->_NET_WM_ACTION_RESIZE);
    }

    ewmh_.set_allowed_actions(window, actions);
}

Geometry WindowManager::current_window_geometry(xcb_window_t window) const
{
    Geometry fallback = { 0, 0, 300, 200 };
    if (auto const* client = get_client(window); client && client->kind == Client::Kind::Floating)
        fallback = client->floating_geometry;

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
    if (!client || client->kind != Client::Kind::Tiled || client->monitor >= monitors_.size())
        return;

    size_t monitor_idx = client->monitor;
    size_t workspace_idx = client->workspace;
    remove_tiled_from_workspace(window, monitor_idx, workspace_idx);

    client->kind = Client::Kind::Floating;
    client->mru_order = next_mru_order_++;
    client->suppress_next_configure_request = false;

    Geometry geometry = current_window_geometry(window);
    if (client->hidden || geometry.x <= OFF_SCREEN_X / 2)
    {
        geometry = floating::place_floating(
            monitors_[monitor_idx].working_area(),
            geometry.width,
            geometry.height,
            std::nullopt
        );
    }
    client->floating_geometry = geometry;

    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE
                          | XCB_EVENT_MASK_BUTTON_PRESS };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    update_allowed_actions(window);
}

void WindowManager::convert_window_to_tiled(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client || client->kind != Client::Kind::Floating || client->monitor >= monitors_.size())
        return;

    client->kind = Client::Kind::Tiled;
    client->suppress_next_configure_request = false;
    size_t workspace_idx = std::min(client->workspace, monitors_[client->monitor].workspaces.size() - 1);
    add_tiled_to_workspace(window, client->monitor, workspace_idx);

    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    update_allowed_actions(window);
}

void WindowManager::toggle_window_float(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;
    if (client->fullscreen || client->iconic || client->layer == WindowLayer::Overlay)
        return;
    if (client->kind != Client::Kind::Tiled && client->kind != Client::Kind::Floating)
        return;
    if (showing_desktop_)
        return;

    size_t monitor_idx = client->monitor;
    if (monitor_idx >= monitors_.size())
        return;

    if (client->kind == Client::Kind::Tiled)
    {
        convert_window_to_floating(window);
        client = get_client(window);
        if (!client)
            return;

        if (client->float_restore.has_value())
        {
            client->floating_geometry = *client->float_restore;
            client->float_restore = std::nullopt;
        }
    }
    else
    {
        client->float_restore = client->maximize_restore.value_or(client->floating_geometry);

        if (client->maximized_horz || client->maximized_vert)
        {
            client->maximized_horz = false;
            client->maximized_vert = false;
            client->maximize_restore = std::nullopt;
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_HORZ, false);
            ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_VERT, false);
        }

        convert_window_to_tiled(window);
        client = get_client(window);
        if (!client)
            return;
    }

    sync_visibility_for_monitor(monitor_idx);
    rearrange_monitor(monitors_[monitor_idx]);
    flush_and_drain_crossing();

    LWM_ASSERT_INVARIANTS(clients_, monitors_);
    focus_any_window(window);
}

void WindowManager::apply_rule_result_to_window(xcb_window_t window, WindowRuleResult const& rule_result)
{
    if (rule_result.fullscreen.has_value() && !*rule_result.fullscreen)
        set_fullscreen(window, false);

    auto* client = get_client(window);
    if (!client)
        return;

    bool wants_overlay = rule_result.layer == WindowLayer::Overlay;
    if (wants_overlay || rule_result.floating.has_value())
    {
        if (wants_overlay || *rule_result.floating)
            convert_window_to_floating(window);
        else
            convert_window_to_tiled(window);
        client = get_client(window);
        if (!client)
            return;
    }

    auto move_window = [this](xcb_window_t id, size_t target_monitor, size_t target_workspace)
    {
        auto* movable = get_client(id);
        if (!movable || target_monitor >= monitors_.size())
            return;

        target_workspace = std::min(target_workspace, monitors_[target_monitor].workspaces.size() - 1);
        if (movable->kind == Client::Kind::Floating)
        {
            bool monitor_changed = movable->monitor != target_monitor;
            movable->monitor = target_monitor;
            movable->workspace = target_workspace;
            if (monitor_changed)
            {
                movable->floating_geometry = floating::place_floating(
                    monitors_[target_monitor].working_area(),
                    movable->floating_geometry.width,
                    movable->floating_geometry.height,
                    std::nullopt
                );
            }

            if (movable->sticky)
                ewmh_.set_window_desktop(id, 0xFFFFFFFF);
            else
                ewmh_.set_window_desktop(id, get_ewmh_desktop_index(target_monitor, target_workspace));
            return;
        }

        if (movable->monitor == target_monitor && movable->workspace == target_workspace)
            return;

        size_t source_monitor = movable->monitor;
        size_t source_workspace = movable->workspace;
        remove_tiled_from_workspace(id, source_monitor, source_workspace);
        add_tiled_to_workspace(id, target_monitor, target_workspace);
        if (id == active_window_)
            workspace_policy::set_workspace_focus(monitors_[target_monitor].workspaces[target_workspace], id);
    };

    size_t target_monitor = client->monitor;
    size_t target_workspace = client->workspace;
    if (rule_result.target_monitor.has_value())
        target_monitor = *rule_result.target_monitor;
    if (rule_result.target_workspace.has_value())
        target_workspace = *rule_result.target_workspace;
    if (rule_result.target_monitor.has_value() || rule_result.target_workspace.has_value())
    {
        move_window(window, target_monitor, target_workspace);
        client = get_client(window);
        if (!client)
            return;
    }

    if (client->kind == Client::Kind::Floating && client->monitor < monitors_.size()
        && rule_result.layer != WindowLayer::Overlay)
    {
        if (rule_result.geometry.has_value())
            client->floating_geometry = *rule_result.geometry;

        if (rule_result.center)
        {
            Geometry area = monitors_[client->monitor].working_area();
            client->floating_geometry.x =
                area.x + static_cast<int16_t>((area.width - client->floating_geometry.width) / 2);
            client->floating_geometry.y =
                area.y + static_cast<int16_t>((area.height - client->floating_geometry.height) / 2);
        }

        client->suppress_next_configure_request = rule_result.geometry.has_value() || rule_result.center;
    }

    if (rule_result.skip_taskbar.has_value())
        set_client_skip_taskbar(window, *rule_result.skip_taskbar);
    if (rule_result.skip_pager.has_value())
        set_client_skip_pager(window, *rule_result.skip_pager);
    if (rule_result.borderless.has_value())
        set_window_borderless(window, *rule_result.borderless);
    if (rule_result.layer.has_value())
        set_window_layer(window, *rule_result.layer);
    if (rule_result.sticky.has_value())
        set_window_sticky(window, *rule_result.sticky);
    if (rule_result.above.has_value())
        set_window_above(window, *rule_result.above);
    if (rule_result.below.has_value())
        set_window_below(window, *rule_result.below);
    if (rule_result.fullscreen.has_value() && *rule_result.fullscreen)
        set_fullscreen(window, true);

    client = get_client(window);
    if (!client || client->kind != Client::Kind::Floating || client->hidden)
        return;

    bool visible = visibility_policy::is_window_visible(
        showing_desktop_,
        client->iconic,
        client->sticky,
        client->monitor,
        client->workspace,
        monitors_
    );
    if (!visible)
        return;

    if (client->fullscreen)
        apply_fullscreen_if_needed(window);
    else if (client->maximized_horz || client->maximized_vert)
        apply_maximized_geometry(window);
    else
        apply_floating_geometry(window);
}

void WindowManager::reapply_rules_to_existing_windows()
{
    std::vector<std::pair<uint64_t, xcb_window_t>> ordered;
    ordered.reserve(clients_.size());

    for (auto const& [window, client] : clients_)
    {
        if (client.kind == Client::Kind::Tiled || client.kind == Client::Kind::Floating)
            ordered.push_back({ client.order, window });
    }

    std::sort(ordered.begin(), ordered.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    for (auto const& [order, window] : ordered)
    {
        (void)order;

        auto const* client = get_client(window);
        if (!client)
            continue;

        WindowMatchInfo match_info{
            .wm_class = client->wm_class,
            .wm_class_name = client->wm_class_name,
            .title = client->name,
            .ewmh_type = client->ewmh_type,
            .is_transient = client->transient_for != XCB_NONE,
        };

        auto rule_result = window_rules_.match(match_info, monitors_, config_.workspaces.names);
        if (rule_result.matched)
            apply_rule_result_to_window(window, rule_result);
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
    Workspace ws_template {};
    // Apply configured layout strategy
    if (config_.layout.strategy == "master-stack")
        ws_template.layout_strategy = LayoutStrategy::MasterStack;

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

        auto [classification, scan_rule_result] = classify_managed_window(window);
        (void)scan_rule_result; // Used implicitly via classify; scan path re-evaluates after manage

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
                manage_floating_window(window);
                if (is_restart_)
                    apply_restart_client_state(window);
                reevaluate_managed_window(window);
                break;
            }

            case WindowClassification::Kind::Tiled:
            {
                manage_window(window);
                if (is_restart_)
                    apply_restart_client_state(window);
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
                client.above = true;
            else if (state == ewmh->_NET_WM_STATE_BELOW)
                client.below = true;
            else if (state == ewmh->_NET_WM_STATE_STICKY)
                client.sticky = true;
            else if (state == ewmh->_NET_WM_STATE_MODAL)
                client.modal = true;
            else if (state == ewmh->_NET_WM_STATE_SKIP_TASKBAR)
                client.skip_taskbar = true;
            else if (state == ewmh->_NET_WM_STATE_SKIP_PAGER)
                client.skip_pager = true;
            else if (state == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION)
                client.demands_attention = true;
        }
        xcb_ewmh_get_atoms_reply_wipe(&initial_state);
    }
}

ClassificationResult WindowManager::classify_managed_window(xcb_window_t window)
{
    bool has_transient = transient_for_window(window).has_value();
    auto classification = ewmh_.classify_window(window, has_transient);

    // Use cached client data when available to avoid X round-trips.
    // On initial manage the client doesn't exist yet, so we fall back to X reads.
    auto const* existing = get_client(window);
    std::string instance_name, class_name, title;
    WindowType ewmh_type;
    if (existing)
    {
        class_name = existing->wm_class;
        instance_name = existing->wm_class_name;
        title = existing->name;
        ewmh_type = existing->ewmh_type;
    }
    else
    {
        auto wm_class = get_wm_class(window);
        instance_name = wm_class.first;
        class_name = wm_class.second;
        title = get_window_name(window);
        ewmh_type = ewmh_.get_window_type_enum(window);
    }
    WindowMatchInfo match_info{ .wm_class = class_name,
                                .wm_class_name = instance_name,
                                .title = title,
                                .ewmh_type = ewmh_type,
                                .is_transient = has_transient };
    auto rule_result = window_rules_.match(match_info, monitors_, config_.workspaces.names);

    if (rule_result.matched && classification.kind != WindowClassification::Kind::Dock
        && classification.kind != WindowClassification::Kind::Desktop
        && classification.kind != WindowClassification::Kind::Popup)
    {
        if (rule_result.floating.has_value())
        {
            classification.kind =
                *rule_result.floating ? WindowClassification::Kind::Floating : WindowClassification::Kind::Tiled;
        }
        if (rule_result.skip_taskbar.has_value())
            classification.skip_taskbar = *rule_result.skip_taskbar;
        if (rule_result.skip_pager.has_value())
            classification.skip_pager = *rule_result.skip_pager;
        if (rule_result.above.has_value() && *rule_result.above)
            classification.above = true;
        if (rule_result.layer == WindowLayer::Overlay)
        {
            classification.kind = WindowClassification::Kind::Floating;
            classification.skip_taskbar = true;
            classification.skip_pager = true;
            classification.above = false;
        }
    }

    return { classification, rule_result };
}

void WindowManager::refresh_user_time_tracking(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;

    client->user_time_window = XCB_NONE;

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
                    client->user_time_window = time_window;
                    if (time_window != window)
                        ensure_property_change_mask(conn_, time_window);
                }
            }
            free(reply);
        }
    }

    client->user_time = get_user_time(window);
}


bool WindowManager::sync_kind_and_layer(
    xcb_window_t window,
    WindowClassification::Kind desired_kind,
    WindowRuleResult const& rule_result)
{
    auto* client = get_client(window);
    if (!client)
        return false;

    WindowLayer desired_layer = rule_result.layer.value_or(WindowLayer::Normal);
    if (desired_layer == WindowLayer::Overlay)
    {
        if (client->kind != Client::Kind::Floating)
            convert_window_to_floating(window);
        set_window_layer(window, WindowLayer::Overlay);
    }
    else
    {
        if (client->layer == WindowLayer::Overlay)
            set_window_layer(window, WindowLayer::Normal);

        client = get_client(window);
        if (!client)
            return false;

        if (desired_kind == WindowClassification::Kind::Floating && client->kind == Client::Kind::Tiled)
            convert_window_to_floating(window);
        else if (desired_kind == WindowClassification::Kind::Tiled && client->kind == Client::Kind::Floating)
            convert_window_to_tiled(window);
    }

    return get_client(window) != nullptr;
}

void WindowManager::relocate_to_transient_parent(xcb_window_t window, xcb_window_t previous_transient_for)
{
    auto* client = get_client(window);
    if (!client || previous_transient_for == client->transient_for || client->transient_for == XCB_NONE)
        return;

    auto parent_monitor = monitor_index_for_window(client->transient_for);
    if (!parent_monitor)
        return;
    auto parent_workspace = workspace_index_for_window(client->transient_for);
    if (!parent_workspace)
        return;

    if (client->kind == Client::Kind::Tiled)
    {
        remove_tiled_from_workspace(window, client->monitor, client->workspace);
        add_tiled_to_workspace(window, *parent_monitor, *parent_workspace);
    }
    else
    {
        assign_window_workspace(window, *parent_monitor, *parent_workspace);

        Geometry geometry = current_window_geometry(window);
        Geometry parent_geometry = current_window_geometry(client->transient_for);
        client->floating_geometry = floating::place_floating(
            monitors_[*parent_monitor].working_area(),
            std::max<uint16_t>(1, geometry.width),
            std::max<uint16_t>(1, geometry.height),
            parent_geometry
        );
    }
}

void WindowManager::apply_classification_state(
    xcb_window_t window,
    WindowClassification const& classification,
    WindowRuleResult const& rule_result,
    bool has_transient)
{
    auto* client = get_client(window);
    if (!client)
        return;

    auto state_flags = ewmh_.get_window_state_flags(window);

    auto desired = classification_policy::compute_desired_state({
        .classification_skip_taskbar = classification.skip_taskbar,
        .classification_skip_pager = classification.skip_pager,
        .classification_above = classification.above,
        .ewmh_skip_taskbar = state_flags.skip_taskbar,
        .ewmh_skip_pager = state_flags.skip_pager,
        .ewmh_sticky = state_flags.sticky,
        .ewmh_modal = state_flags.modal,
        .ewmh_above = state_flags.above,
        .ewmh_below = state_flags.below,
        .rule_skip_taskbar = rule_result.skip_taskbar,
        .rule_skip_pager = rule_result.skip_pager,
        .rule_sticky = rule_result.sticky,
        .rule_above = rule_result.above,
        .rule_below = rule_result.below,
        .rule_borderless = rule_result.borderless,
        .has_transient = has_transient,
        .is_sticky_desktop = is_sticky_desktop(window),
        .layer = client->layer,
    });

    if (client->skip_taskbar != desired.skip_taskbar)
        set_client_skip_taskbar(window, desired.skip_taskbar);
    if (client->skip_pager != desired.skip_pager)
        set_client_skip_pager(window, desired.skip_pager);
    if (client->sticky != desired.sticky)
        set_window_sticky(window, desired.sticky);
    if (client->modal != desired.modal)
        set_window_modal(window, desired.modal);
    if (client->above != desired.above)
        set_window_above(window, desired.above);
    if (client->below != desired.below)
        set_window_below(window, desired.below);
    if (client->borderless != desired.borderless)
        set_window_borderless(window, desired.borderless);

    update_allowed_actions(window);
}

void WindowManager::sync_managed_window_classification(xcb_window_t window, ClassificationResult const& result)
{
    auto const& classification = result.classification;
    auto const& rule_result = result.rule_result;

    auto* client = get_client(window);
    if (!client)
        return;

    Client::Kind previous_kind = client->kind;
    size_t previous_monitor = client->monitor;
    size_t previous_workspace = client->workspace;
    xcb_window_t previous_transient_for = client->transient_for;

    auto transient = transient_for_window(window);
    bool has_transient = transient.has_value();
    client->transient_for = transient.value_or(XCB_NONE);

    // If classification isn't tiled/floating, only transient restacking matters
    WindowClassification::Kind desired_kind = classification.kind;
    if (desired_kind != WindowClassification::Kind::Tiled && desired_kind != WindowClassification::Kind::Floating)
    {
        if (previous_transient_for != client->transient_for)
        {
            restack_transients(previous_transient_for);
            restack_transients(client->transient_for);
        }
        return;
    }

    // --- Phase 3: Apply kind, layer, and state flag changes ---
    if (!sync_kind_and_layer(window, desired_kind, rule_result))
        return;
    relocate_to_transient_parent(window, previous_transient_for);
    apply_classification_state(window, classification, rule_result, has_transient);

    client = get_client(window);
    if (!client)
        return;

    if (rule_result.fullscreen.has_value() && !*rule_result.fullscreen)
        set_fullscreen(window, false);

    auto move_window = [this](xcb_window_t id, size_t target_monitor, size_t target_workspace)
    {
        auto* movable = get_client(id);
        if (!movable || target_monitor >= monitors_.size())
            return;

        target_workspace = std::min(target_workspace, monitors_[target_monitor].workspaces.size() - 1);
        if (movable->kind == Client::Kind::Floating)
        {
            bool monitor_changed = movable->monitor != target_monitor;
            movable->monitor = target_monitor;
            movable->workspace = target_workspace;
            if (monitor_changed)
            {
                movable->floating_geometry = floating::place_floating(
                    monitors_[target_monitor].working_area(),
                    movable->floating_geometry.width,
                    movable->floating_geometry.height,
                    std::nullopt
                );
            }

            if (movable->sticky)
                ewmh_.set_window_desktop(id, 0xFFFFFFFF);
            else
                ewmh_.set_window_desktop(id, get_ewmh_desktop_index(target_monitor, target_workspace));
            return;
        }

        if (movable->monitor == target_monitor && movable->workspace == target_workspace)
            return;

        size_t source_monitor = movable->monitor;
        size_t source_workspace = movable->workspace;
        remove_tiled_from_workspace(id, source_monitor, source_workspace);
        add_tiled_to_workspace(id, target_monitor, target_workspace);
        if (id == active_window_)
            workspace_policy::set_workspace_focus(monitors_[target_monitor].workspaces[target_workspace], id);
    };

    size_t target_monitor = client->monitor;
    size_t target_workspace = client->workspace;
    if (rule_result.target_monitor.has_value())
        target_monitor = *rule_result.target_monitor;
    if (rule_result.target_workspace.has_value())
        target_workspace = *rule_result.target_workspace;
    if (rule_result.target_monitor.has_value() || rule_result.target_workspace.has_value())
    {
        move_window(window, target_monitor, target_workspace);
        client = get_client(window);
        if (!client)
            return;
    }

    // Title-based reevaluation should honor the full rule result, including
    // floating placement directives such as geometry and centering.
    if (client->kind == Client::Kind::Floating && client->monitor < monitors_.size()
        && rule_result.layer != WindowLayer::Overlay)
    {
        if (rule_result.geometry.has_value())
            client->floating_geometry = *rule_result.geometry;

        if (rule_result.center)
        {
            Geometry area = monitors_[client->monitor].working_area();
            client->floating_geometry.x =
                area.x + static_cast<int16_t>((area.width - client->floating_geometry.width) / 2);
            client->floating_geometry.y =
                area.y + static_cast<int16_t>((area.height - client->floating_geometry.height) / 2);
        }

        client->suppress_next_configure_request = rule_result.geometry.has_value() || rule_result.center;
    }

    if (rule_result.fullscreen.has_value() && *rule_result.fullscreen)
        set_fullscreen(window, true);

    if (previous_transient_for != client->transient_for)
    {
        restack_transients(previous_transient_for);
        restack_transients(client->transient_for);
    }

    // --- Phase 4: Sync visibility and layout for affected monitors ---
    size_t current_monitor = client->monitor;
    bool monitor_changed = previous_monitor != current_monitor;
    bool workspace_changed = previous_workspace != client->workspace;
    bool kind_changed = previous_kind != client->kind;

    // Sync visibility on all affected monitors
    if (previous_monitor < monitors_.size())
    {
        update_fullscreen_owner_after_visibility_change(previous_monitor);
        sync_visibility_for_monitor(previous_monitor);
    }
    if (monitor_changed && current_monitor < monitors_.size())
    {
        update_fullscreen_owner_after_visibility_change(current_monitor);
        sync_visibility_for_monitor(current_monitor);
    }

    // Rearrange/restack previous monitor if window moved away
    if (previous_monitor < monitors_.size() && (monitor_changed || workspace_changed || kind_changed))
    {
        if (previous_kind == Client::Kind::Tiled)
            rearrange_monitor(monitors_[previous_monitor]);
        else if (monitor_changed)
            restack_monitor_layers(previous_monitor);
    }

    if (current_monitor >= monitors_.size())
        return;

    // Rearrange/apply geometry on current monitor
    if (client->kind == Client::Kind::Tiled)
    {
        if (kind_changed || monitor_changed || workspace_changed)
            rearrange_monitor(monitors_[current_monitor]);
    }
    else
    {
        if (!client->hidden && should_be_visible(window))
        {
            if (client->fullscreen)
                apply_fullscreen_if_needed(window);
            else if (client->maximized_horz || client->maximized_vert)
                apply_maximized_geometry(window);
            else
                apply_floating_geometry(window);
        }
        restack_monitor_layers(current_monitor);
    }

    if (window == active_window_ && !is_focus_eligible(window))
    {
        if (current_monitor == focused_monitor_
            && (client->sticky || client->workspace == monitors_[current_monitor].current_workspace))
        {
            focus_or_fallback(monitors_[current_monitor], false);
        }
        else
        {
            clear_focus();
        }
    }

    flush_and_drain_crossing();
}

void WindowManager::reevaluate_managed_window(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;

    if (client->kind != Client::Kind::Tiled && client->kind != Client::Kind::Floating)
        return;

    auto result = classify_managed_window(window);

    if (!client->scratchpad_name.has_value() && !client->in_scratchpad)
    {
        auto scratchpad_match = match_scratchpad_for_window(window, result.rule_result);
        if (scratchpad_match)
        {
            auto* state = find_named_scratchpad(*scratchpad_match);
            if (state && state->window == XCB_NONE && state->pending_launch)
            {
                client->scratchpad_name = scratchpad_match;
                finalize_scratchpad_claim(window, *state, *scratchpad_match);
                return;
            }
        }
    }

    sync_managed_window_classification(window, result);
}

void WindowManager::apply_post_manage_states(xcb_window_t window, bool has_transient)
{
    auto state_flags = ewmh_.get_window_state_flags(window);

    if (has_transient || state_flags.skip_taskbar)
    {
        set_client_skip_taskbar(window, true);
    }
    if (has_transient || state_flags.skip_pager)
    {
        set_client_skip_pager(window, true);
    }

    if (is_sticky_desktop(window))
    {
        set_window_sticky(window, true);
    }
    else if (state_flags.sticky)
    {
        set_window_sticky(window, true);
    }

    if (state_flags.modal)
    {
        set_window_modal(window, true);
    }

    if (state_flags.above)
    {
        set_window_above(window, true);
    }
    else if (state_flags.below)
    {
        set_window_below(window, true);
    }
}

void WindowManager::manage_window(xcb_window_t window, bool start_iconic)
{
    auto [instance_name, class_name] = get_wm_class(window);
    std::string window_name = get_window_name(window);
    auto target = resolve_window_desktop(window);
    size_t target_monitor_idx = target ? target->first : focused_monitor_;
    size_t target_workspace_idx = target ? target->second : monitors_[target_monitor_idx].current_workspace;

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
        client.ewmh_type = ewmh_.get_window_type_enum(window);
        parse_initial_ewmh_state(client);
        client.transient_for = transient_for_window(window).value_or(XCB_NONE);

        clients_[window] = std::move(client);
    }
    cache_focus_hints(window);

    monitors_[target_monitor_idx].workspaces[target_workspace_idx].windows.push_back(window);

    refresh_user_time_tracking(window);

    // Note: We intentionally do NOT select STRUCTURE_NOTIFY on client windows.
    // We receive UnmapNotify/DestroyNotify via root's SubstructureNotifyMask.
    // Selecting STRUCTURE_NOTIFY on clients would cause duplicate UnmapNotify events,
    // leading to incorrect unmanagement of windows during workspace switches (ICCCM compliance).
    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);

    if (auto const* client = get_client(window))
    {
        uint32_t border_width = border_width_for_client(*client);
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);
    }

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { start_iconic ? WM_STATE_ICONIC : WM_STATE_NORMAL, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    if (start_iconic)
    {
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_HIDDEN, true);
    }

    update_sync_state(window);
    update_fullscreen_monitor_state(window);

    ewmh_.set_frame_extents(window, 0, 0, 0, 0);
    uint32_t desktop = get_ewmh_desktop_index(target_monitor_idx, target_workspace_idx);
    ewmh_.set_window_desktop(window, desktop);

    update_allowed_actions(window);

    update_ewmh_client_list();

    // Fetch all EWMH state flags in a single round-trip
    auto manage_state_flags = ewmh_.get_window_state_flags(window);

    // Check fullscreen BEFORE rearrange so window is properly excluded from tiling
    // and gets correct geometry when mapped. Other EWMH states are handled after.
    if (manage_state_flags.fullscreen)
    {
        set_fullscreen(window, true);
    }

    keybinds_.grab_keys(window);

    // With off-screen visibility: map window once when managing, then let
    // sync_visibility decide whether it should be hidden or shown.
    xcb_map_window(conn_.get(), window);

    sync_visibility_for_monitor(target_monitor_idx);
    rearrange_monitor(monitors_[target_monitor_idx]);

    apply_post_manage_states(window, false);

    // Maximized states are geometry-affecting, applied post-map for tiled windows
    // (for floating, these are applied pre-map inline in manage_floating_window)
    if (manage_state_flags.maximized_horz || manage_state_flags.maximized_vert)
    {
        set_window_maximized(window, manage_state_flags.maximized_horz, manage_state_flags.maximized_vert);
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

    release_fullscreen_owner(window);

    size_t mon_idx = client->monitor;
    size_t ws_idx = client->workspace;
    bool was_active = (active_window_ == window);

    if (mon_idx < monitors_.size() && ws_idx < monitors_[mon_idx].workspaces.size())
    {
        auto is_iconic = [this](xcb_window_t w) { return is_client_iconic(w); };
        auto& monitor = monitors_[mon_idx];
        workspace_policy::remove_tiled_window(monitor.workspaces[ws_idx], window, is_iconic);
        clients_.erase(window);
        LWM_ASSERT_INVARIANTS(clients_, monitors_);
        update_ewmh_client_list();
        update_fullscreen_owner_after_visibility_change(mon_idx);
        sync_visibility_for_monitor(mon_idx);
        rearrange_monitor(monitor);

        if (was_active)
        {
            if (ws_idx == monitor.current_workspace && mon_idx == focused_monitor_)
            {
                focus_or_fallback(monitor);
            }
            else
            {
                clear_focus();
            }
        }

        flush_and_drain_crossing();
        return;
    }

    clients_.erase(window);
}

void WindowManager::clear_fullscreen_state(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client || !client->fullscreen)
        return;

    client->fullscreen = false;
    ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN, false);

    // release_fullscreen_owner also removes from per-monitor fullscreen_windows
    release_fullscreen_owner(window);

    if (is_floating_window(window) && client->fullscreen_restore)
        client->floating_geometry = *client->fullscreen_restore;

    client->fullscreen_restore = std::nullopt;
    uint32_t border_width = border_width_for_client(*client);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);

    if (client->kind == Client::Kind::Floating && client->monitor < monitors_.size() && should_be_visible(window)
        && !client->hidden && !is_suppressed_by_fullscreen(window))
    {
        apply_floating_geometry(window);
    }
}

void WindowManager::set_fullscreen(xcb_window_t window, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return; // Only managed clients can be fullscreen

    if (enabled && client->layer == WindowLayer::Overlay)
    {
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_FULLSCREEN, false);
        conn_.flush();
        return;
    }

    if (enabled)
    {
        if (!client->fullscreen && is_floating_window(window))
        {
            client->fullscreen_restore = client->floating_geometry;
        }

        client->fullscreen = true;

        // Track in per-monitor fullscreen list
        if (client->monitor < monitors_.size())
        {
            auto& fs_list = monitors_[client->monitor].fullscreen_windows;
            if (std::ranges::find(fs_list, window) == fs_list.end())
                fs_list.push_back(window);
        }

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

        claim_fullscreen_owner(window);
    }
    else
    {
        if (!client->fullscreen)
            return;

        clear_fullscreen_state(window);
    }

    client = get_client(window);
    if (!client)
        return;

    if (client->monitor < monitors_.size())
    {
        // When fullscreen is disabled, try to select a new owner if another fullscreen window exists
        if (!enabled)
            update_fullscreen_owner_after_visibility_change(client->monitor);

        sync_visibility_for_monitor(client->monitor);
        rearrange_monitor(monitors_[client->monitor]);

        if (client->monitor == focused_monitor_
            && (active_window_ == XCB_NONE || is_suppressed_by_fullscreen(active_window_)))
        {
            focus_or_fallback(monitors_[client->monitor], false);
        }
    }
    flush_and_drain_crossing();
}

void WindowManager::set_window_layer(xcb_window_t window, WindowLayer layer)
{
    auto* client = get_client(window);
    if (!client)
        return;

    if (layer == WindowLayer::Overlay && client->kind != Client::Kind::Floating)
    {
        convert_window_to_floating(window);
        client = get_client(window);
        if (!client)
            return;
    }

    client->layer = layer;

    if (layer == WindowLayer::Overlay)
    {
        if (client->fullscreen)
            set_fullscreen(window, false);

        client->maximized_horz = false;
        client->maximized_vert = false;
        client->maximize_restore = std::nullopt;
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_HORZ, false);
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_MAXIMIZED_VERT, false);
        client->above = false;
        client->below = false;
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_ABOVE, false);
        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_BELOW, false);
        set_client_skip_taskbar(window, true);
        set_client_skip_pager(window, true);
        set_window_borderless(window, true);
        if (!client->sticky)
            set_window_sticky(window, true);
    }

    update_allowed_actions(window);

    if (client->kind == Client::Kind::Floating && !client->hidden)
        apply_floating_geometry(window);
    if (client->monitor < monitors_.size())
        restack_monitor_layers(client->monitor);
    conn_.flush();
}

void WindowManager::set_window_borderless(xcb_window_t window, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return;

    client->borderless = enabled;
    uint32_t border_width = border_width_for_client(*client);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);
    uint32_t color = (border_width > 0) ? border_color_for_client(*client) : conn_.screen()->black_pixel;
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &color);
    update_allowed_actions(window);
    conn_.flush();
}

void WindowManager::set_window_above_below(xcb_window_t window, bool is_above, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return;

    bool& primary = is_above ? client->above : client->below;
    bool& opposite = is_above ? client->below : client->above;
    xcb_atom_t primary_atom = is_above ? ewmh_.get()->_NET_WM_STATE_ABOVE : ewmh_.get()->_NET_WM_STATE_BELOW;
    xcb_atom_t opposite_atom = is_above ? ewmh_.get()->_NET_WM_STATE_BELOW : ewmh_.get()->_NET_WM_STATE_ABOVE;

    if (client->layer == WindowLayer::Overlay)
    {
        primary = false;
        ewmh_.set_window_state(window, primary_atom, false);
        conn_.flush();
        return;
    }

    if (enabled)
    {
        primary = true;
        if (opposite)
        {
            opposite = false;
            ewmh_.set_window_state(window, opposite_atom, false);
        }
    }
    else
    {
        primary = false;
    }

    ewmh_.set_window_state(window, primary_atom, enabled);
    if (client->monitor < monitors_.size())
        restack_monitor_layers(client->monitor);
    conn_.flush();
}

void WindowManager::set_window_sticky(xcb_window_t window, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return;

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

    if (client->monitor < monitors_.size())
    {
        update_fullscreen_owner_after_visibility_change(client->monitor);
        sync_visibility_for_monitor(client->monitor);
        if (client->kind == Client::Kind::Tiled)
            rearrange_monitor(monitors_[client->monitor]);
        else
            restack_monitor_layers(client->monitor);
    }

    flush_and_drain_crossing();
}

void WindowManager::set_window_maximized(xcb_window_t window, bool horiz, bool vert)
{
    auto* client = get_client(window);
    if (!client)
        return;

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
                if (is_floating_window(window))
                {
                    client->floating_geometry = *client->maximize_restore;
                    if (should_be_visible(window) && !client->hidden)
                    {
                        apply_floating_geometry(window);
                    }
                }
                client->maximize_restore = std::nullopt;
            }
        }
        else if (is_floating_window(window))
        {
            if (!client->maximize_restore)
            {
                client->maximize_restore = client->floating_geometry;
            }
            apply_maximized_geometry(window);
        }
    }

    conn_.flush();
}

void WindowManager::apply_maximized_geometry(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;

    if (!is_floating_window(window))
        return;
    if (client->monitor >= monitors_.size())
        return;

    Geometry base = client->floating_geometry;
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

    client->floating_geometry = base;
    if (should_be_visible(window) && !client->hidden)
    {
        apply_floating_geometry(window);
    }
}


void WindowManager::set_window_modal(xcb_window_t window, bool enabled)
{
    auto* client = get_client(window);
    if (!client)
        return;

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

void WindowManager::set_simple_client_state(xcb_window_t window, bool Client::*field, xcb_atom_t atom, bool enabled)
{
    if (auto* c = get_client(window))
        c->*field = enabled;
    ewmh_.set_window_state(window, atom, enabled);
}

void WindowManager::set_client_skip_taskbar(xcb_window_t window, bool enabled)
{
    set_simple_client_state(window, &Client::skip_taskbar, ewmh_.get()->_NET_WM_STATE_SKIP_TASKBAR, enabled);
}

void WindowManager::set_client_skip_pager(xcb_window_t window, bool enabled)
{
    set_simple_client_state(window, &Client::skip_pager, ewmh_.get()->_NET_WM_STATE_SKIP_PAGER, enabled);
}

void WindowManager::set_client_demands_attention(xcb_window_t window, bool enabled)
{
    auto const* client = get_client(window);
    if (client && client->demands_attention == enabled)
        return;

    set_simple_client_state(window, &Client::demands_attention, ewmh_.get()->_NET_WM_STATE_DEMANDS_ATTENTION, enabled);

    // Sync ICCCM WM_HINTS urgency flag so panels that check WM_HINTS (e.g. polybar
    // xworkspaces) also see the urgency state.
    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), window), &hints, nullptr))
    {
        if (enabled)
            hints.flags |= XUrgencyHint;
        else
            hints.flags &= ~XUrgencyHint;
        xcb_icccm_set_wm_hints(conn_.get(), window, &hints);
    }
    else if (enabled)
    {
        xcb_icccm_wm_hints_t new_hints = {};
        new_hints.flags = XUrgencyHint;
        xcb_icccm_set_wm_hints(conn_.get(), window, &new_hints);
    }

    if (client && window != active_window_ && border_width_for_client(*client) > 0)
    {
        uint32_t color = border_color_for_client(*client);
        xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &color);
    }

    // Re-publish client list so EWMH panels (e.g. polybar) re-check urgency.
    update_ewmh_client_list();
    conn_.flush();
}

std::optional<xcb_window_t> WindowManager::resolve_notification_target(NotificationAttentionRequest const& req) const
{
    // Name-based resolution: case-insensitive match against wm_class_name and wm_class,
    // exclude active_window_, pick highest mru_order (most recently interacted with).
    auto ci_equal = [](std::string_view a, std::string_view b) -> bool
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    };

    auto best_match = [&](std::string_view name) -> std::optional<xcb_window_t>
    {
        xcb_window_t best = XCB_NONE;
        uint64_t best_mru = 0;
        for (auto const& [wid, client] : clients_)
        {
            if (wid == active_window_)
                continue;
            if (client.kind != Client::Kind::Tiled && client.kind != Client::Kind::Floating)
                continue;
            if (!ci_equal(client.wm_class_name, name) && !ci_equal(client.wm_class, name))
                continue;
            if (best == XCB_NONE || client.mru_order > best_mru)
            {
                best = wid;
                best_mru = client.mru_order;
            }
        }
        if (best != XCB_NONE)
            return best;
        return std::nullopt;
    };

    if (!req.desktop_entry.empty())
    {
        if (auto w = best_match(req.desktop_entry))
            return w;
    }

    if (!req.app_name.empty())
    {
        if (auto w = best_match(req.app_name))
            return w;
    }

    return std::nullopt;
}

std::string WindowManager::handle_notification_attention(NotificationAttentionRequest const& req)
{
    // Exact window ID: skip if already focused (the user is looking at it).
    if (req.window != XCB_NONE)
    {
        auto const* client = get_client(req.window);
        if (!client || (client->kind != Client::Kind::Tiled && client->kind != Client::Kind::Floating))
            return ok_reply("no-match");
        if (req.window == active_window_)
            return ok_reply("skipped-active");
        set_client_demands_attention(req.window, true);
        return ok_reply(std::to_string(req.window));
    }

    // Name-based fallback: single best match.
    auto target = resolve_notification_target(req);
    if (!target)
        return ok_reply("no-match");
    set_client_demands_attention(*target, true);
    return ok_reply(std::to_string(*target));
}

void WindowManager::apply_fullscreen_if_needed(xcb_window_t window)
{
    if (!is_client_fullscreen(window))
    {
        LOG_DEBUG("apply_fullscreen_if_needed({:#x}): NOT fullscreen, returning early", window);
        return;
    }
    if (is_suppressed_by_fullscreen(window))
    {
        LOG_DEBUG("apply_fullscreen_if_needed({:#x}): suppressed by fullscreen owner, returning early", window);
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
    if (!client->sticky && client->workspace != monitors_[*monitor_idx].current_workspace)
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
}

void WindowManager::set_fullscreen_monitors(xcb_window_t window, FullscreenMonitors const& monitors)
{
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

    client->iconic = true;
    set_iconic_state(window, true);

    if (client->monitor < monitors_.size())
    {
        release_fullscreen_owner(window);
        update_fullscreen_owner_after_visibility_change(client->monitor);
        sync_visibility_for_monitor(client->monitor);
        if (client->kind == Client::Kind::Tiled)
            rearrange_monitor(monitors_[client->monitor]);
        else
            restack_monitor_layers(client->monitor);
    }

    if (active_window_ == window)
    {
        bool was_visible = visibility_policy::is_window_visible(
            showing_desktop_, false, client->sticky, client->monitor, client->workspace, monitors_
        );
        if (client->monitor == focused_monitor_ && was_visible)
        {
            focus_or_fallback(monitors_[client->monitor]);
        }
        else
        {
            clear_focus();
        }
    }

    flush_and_drain_crossing();
}

void WindowManager::deiconify_window(xcb_window_t window, bool focus)
{
    auto* client = get_client(window);
    if (!client)
        return;

    client->iconic = false;
    set_iconic_state(window, false);

    if (client->monitor < monitors_.size())
    {
        if (client->fullscreen)
            claim_fullscreen_owner(window);
        else
        {
            update_fullscreen_owner_after_visibility_change(client->monitor);
        }
        sync_visibility_for_monitor(client->monitor);
        if (client->kind == Client::Kind::Tiled)
            rearrange_monitor(monitors_[client->monitor]);
        else
            restack_monitor_layers(client->monitor);
    }

    if (focus && client->monitor == focused_monitor_ && should_be_visible(window))
    {
        focus_any_window(window);
    }

    apply_fullscreen_if_needed(window);
    flush_and_drain_crossing();
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

    // Build the visible tiled window set by reading client.hidden — the
    // authoritative hidden flag is maintained by sync_visibility_for_monitor,
    // which must be called BEFORE rearrange_monitor at every call site.
    std::vector<xcb_window_t> visible_windows;
    visible_windows.reserve(monitor.current().windows.size());
    std::vector<xcb_window_t> fullscreen_windows;
    std::unordered_set<xcb_window_t> seen;

    // Collect visible non-fullscreen tiled windows from the current workspace
    for (xcb_window_t window : monitor.current().windows)
    {
        auto const* client = get_client(window);
        if (!client || client->hidden)
            continue;
        if (client->fullscreen)
        {
            LOG_TRACE("rearrange_monitor: fullscreen window {:#x} excluded from tiling", window);
            fullscreen_windows.push_back(window);
            seen.insert(window);
            continue;
        }
        visible_windows.push_back(window);
        seen.insert(window);
    }

    // Collect sticky tiled windows from other workspaces that are visible
    for (auto const& workspace : monitor.workspaces)
    {
        for (xcb_window_t window : workspace.windows)
        {
            if (seen.contains(window))
                continue;
            auto const* client = get_client(window);
            if (!client || !client->sticky || client->hidden)
                continue;
            if (client->fullscreen)
            {
                LOG_TRACE("rearrange_monitor: sticky fullscreen window {:#x} excluded from tiling", window);
                fullscreen_windows.push_back(window);
                seen.insert(window);
                continue;
            }
            LOG_TRACE("rearrange_monitor: adding sticky window {:#x}", window);
            visible_windows.push_back(window);
            seen.insert(window);
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

    auto& ws = monitor.current();

    // Collect old geometries so arrange can skip unchanged windows
    std::vector<Geometry> old_geometries;
    if (geometry_only)
    {
        old_geometries.reserve(visible_windows.size());
        for (auto w : visible_windows)
        {
            if (auto const* c = get_client(w))
                old_geometries.push_back(c->tiled_geometry);
            else
                old_geometries.push_back({});
        }
    }

    auto applied = layout_.arrange(
        visible_windows, monitor.working_area(), ws.layout_strategy, ws.split_ratios,
        geometry_only ? &old_geometries : nullptr);
    for (size_t i = 0; i < visible_windows.size() && i < applied.size(); ++i)
    {
        if (auto* c = get_client(visible_windows[i]))
            c->tiled_geometry = applied[i];
    }

    // Apply fullscreen geometry for visible fullscreen tiled windows
    for (xcb_window_t window : fullscreen_windows)
    {
        LOG_DEBUG("rearrange_monitor: applying fullscreen geometry for {:#x}", window);
        apply_fullscreen_if_needed(window);
    }

    if (!geometry_only)
        restack_monitor_layers(monitor_idx);
    conn_.flush();

    LOG_TRACE("rearrange_monitor: DONE");
}

void WindowManager::rearrange_all_monitors()
{
    sync_visibility_all();
    for (auto& monitor : monitors_)
    {
        rearrange_monitor(monitor);
    }
}

void WindowManager::launch_program(CommandConfig const& command)
{
    if (fork() == 0)
    {
        setsid();

        if (command.kind == CommandConfig::Kind::Shell)
        {
            execl("/bin/sh", "sh", "-c", command.shell.c_str(), nullptr);
            _exit(1);
        }

        std::vector<char*> argv;
        argv.reserve(command.argv.size() + 1);
        for (auto const& arg : command.argv)
        {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv.front(), argv.data());
        _exit(1);
    }
}

void WindowManager::adjust_master_ratio(double delta)
{
    auto& ws = focused_monitor().current();
    double min_ratio = config_.layout.min_ratio;

    // Root split address = {depth=0, path=0}
    SplitAddress root_addr { 0, 0 };

    double current = config_.layout.default_ratio;
    if (auto it = ws.split_ratios.find(root_addr); it != ws.split_ratios.end())
        current = it->second;

    double clamped = std::clamp(current + delta, min_ratio, 1.0 - min_ratio);
    ws.split_ratios[root_addr] = clamped;

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

#define DEFINE_CLIENT_STATE_QUERY(method, field)          \
    bool WindowManager::method(xcb_window_t window) const \
    {                                                     \
        if (auto const* c = get_client(window))           \
            return c->field;                              \
        return false;                                     \
    }

DEFINE_CLIENT_STATE_QUERY(is_client_fullscreen, fullscreen)
bool WindowManager::is_client_overlay(xcb_window_t window) const
{
    auto const* client = get_client(window);
    return client && client->layer == WindowLayer::Overlay;
}
DEFINE_CLIENT_STATE_QUERY(is_client_iconic, iconic)
DEFINE_CLIENT_STATE_QUERY(is_client_sticky, sticky)
DEFINE_CLIENT_STATE_QUERY(is_client_above, above)
DEFINE_CLIENT_STATE_QUERY(is_client_below, below)
DEFINE_CLIENT_STATE_QUERY(is_client_maximized_horz, maximized_horz)
DEFINE_CLIENT_STATE_QUERY(is_client_maximized_vert, maximized_vert)

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

bool WindowManager::should_be_visible(xcb_window_t window) const
{
    auto const* client = get_client(window);
    if (!client)
        return false;
    return visibility_policy::is_window_visible(
        showing_desktop_,
        client->iconic,
        client->sticky,
        client->monitor,
        client->workspace,
        monitors_
    );
}

bool WindowManager::is_physically_visible(xcb_window_t window) const
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

void WindowManager::release_fullscreen_owner(xcb_window_t window)
{
    auto const* client = get_client(window);
    if (!client || client->monitor >= monitors_.size())
        return;
    auto& monitor = monitors_[client->monitor];
    if (monitor.fullscreen_owner == window)
        monitor.fullscreen_owner = XCB_NONE;
    std::erase(monitor.fullscreen_windows, window);
}

void WindowManager::claim_fullscreen_owner(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client || client->monitor >= monitors_.size())
        return;
    auto& monitor = monitors_[client->monitor];
    // Do NOT clear_fullscreen_state on the old owner — just reassign ownership.
    // The old owner keeps its fullscreen flag and EWMH property, and will be
    // suppressed (hidden off-screen) by is_suppressed_by_fullscreen() + sync_visibility.
    auto& fs_list = monitor.fullscreen_windows;
    if (std::ranges::find(fs_list, window) == fs_list.end())
        fs_list.push_back(window);
    monitor.fullscreen_owner = window;
}

void WindowManager::update_fullscreen_owner_after_visibility_change(size_t monitor_idx)
{
    if (monitor_idx >= monitors_.size())
        return;
    auto& monitor = monitors_[monitor_idx];

    // Check if current owner is still valid
    if (monitor.fullscreen_owner != XCB_NONE)
    {
        auto* owner = get_client(monitor.fullscreen_owner);
        if (owner && owner->fullscreen && !owner->iconic && owner->monitor == monitor_idx
            && should_be_visible(monitor.fullscreen_owner) && !showing_desktop_)
        {
            return; // Current owner is still valid
        }
        monitor.fullscreen_owner = XCB_NONE;
    }

    if (showing_desktop_)
        return;

    // Find a new owner from the per-monitor fullscreen tracking list
    xcb_window_t best = XCB_NONE;
    uint64_t best_order = 0;
    for (xcb_window_t id : monitor.fullscreen_windows)
    {
        auto const* client = get_client(id);
        if (!client || !client->fullscreen || client->iconic)
            continue;
        if (!should_be_visible(id))
            continue;
        if (best == XCB_NONE || client->order > best_order)
        {
            best = id;
            best_order = client->order;
        }
    }
    monitor.fullscreen_owner = best;
}

bool WindowManager::is_suppressed_by_fullscreen(xcb_window_t window) const
{
    auto const* client = get_client(window);
    if (!client)
        return false;
    if (client->kind != Client::Kind::Tiled && client->kind != Client::Kind::Floating)
        return false;
    if (client->layer == WindowLayer::Overlay)
        return false;
    if (client->monitor >= monitors_.size())
        return false;
    if (client->iconic || !should_be_visible(window))
        return false;

    xcb_window_t owner = monitors_[client->monitor].fullscreen_owner;
    if (owner == XCB_NONE || owner == window)
        return false;

    if (client->transient_for == owner)
        return false;

    return true;
}

int WindowManager::compute_stack_layer(xcb_window_t window, Client const& client) const
{
    if (client.layer == WindowLayer::Overlay)
        return static_cast<int>(StackLayer::Overlay);
    if (is_suppressed_by_fullscreen(window))
        return static_cast<int>(StackLayer::Below);
    if (client.fullscreen)
        return static_cast<int>(StackLayer::Fullscreen);
    if (client.above || client.modal)
        return static_cast<int>(StackLayer::Above);
    if (client.below)
        return static_cast<int>(StackLayer::Below);
    return static_cast<int>(StackLayer::Normal);
}

Geometry WindowManager::overlay_geometry_for_window(xcb_window_t window) const
{
    if (monitors_.empty())
        return {};

    auto const* client = get_client(window);
    if (client && client->monitor < monitors_.size())
        return monitors_[client->monitor].geometry();

    if (auto monitor_idx = monitor_index_for_window(window))
        return monitors_[*monitor_idx].geometry();

    return monitors_[0].geometry();
}

void WindowManager::restack_transients(xcb_window_t parent)
{
    if (parent == XCB_NONE)
        return;
    if (!is_physically_visible(parent))
        return;
    if (is_suppressed_by_fullscreen(parent))
        return;

    for (auto const& [fw, client] : clients_)
    {
        if (client.kind != Client::Kind::Floating || client.transient_for != parent)
            continue;
        if (!is_physically_visible(fw))
            continue;
        if (is_suppressed_by_fullscreen(fw))
            continue;

        uint32_t values[2];
        values[0] = parent;
        values[1] = XCB_STACK_MODE_ABOVE;
        uint16_t mask = XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;
        xcb_configure_window(conn_.get(), fw, mask, values);
    }
}

void WindowManager::restack_monitor_layers(size_t monitor_idx)
{
    if (monitor_idx >= monitors_.size())
        return;

    std::vector<xcb_window_t> visible_windows;
    visible_windows.reserve(clients_.size());
    for (auto const& [window, client] : clients_)
    {
        if (client.monitor != monitor_idx)
            continue;
        if (client.kind != Client::Kind::Tiled && client.kind != Client::Kind::Floating)
            continue;
        if (!is_physically_visible(window))
            continue;
        visible_windows.push_back(window);
    }

    std::stable_sort(
        visible_windows.begin(),
        visible_windows.end(),
        [this](xcb_window_t lhs, xcb_window_t rhs)
        {
            auto lit = clients_.find(lhs);
            auto rit = clients_.find(rhs);
            if (lit == clients_.end() || rit == clients_.end())
                return lit != clients_.end();
            auto const& left = lit->second;
            auto const& right = rit->second;
            auto left_key = std::tuple{
                compute_stack_layer(lhs, left),
                left.kind == Client::Kind::Floating ? 1 : 0,
                lhs == active_window_ ? 1 : 0,
                static_cast<long long>(left.order)
            };
            auto right_key = std::tuple{
                compute_stack_layer(rhs, right),
                right.kind == Client::Kind::Floating ? 1 : 0,
                rhs == active_window_ ? 1 : 0,
                static_cast<long long>(right.order)
            };
            return left_key < right_key;
        }
    );

    for (size_t i = 1; i < visible_windows.size(); ++i)
    {
        uint32_t values[2];
        values[0] = visible_windows[i - 1];
        values[1] = XCB_STACK_MODE_ABOVE;
        uint16_t mask = XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;
        xcb_configure_window(conn_.get(), visible_windows[i], mask, values);
    }

    for (xcb_window_t window : visible_windows)
    {
        auto const* client = get_client(window);
        if (!client || client->transient_for != XCB_NONE)
            continue;
        restack_transients(window);
    }

    // Raise overlay windows to the absolute top of the global X stacking order.
    // Per-monitor restacking only orders windows relative to same-monitor siblings,
    // so a newly mapped window on another monitor could end up above the overlay
    // in the global stack.  This final pass guarantees overlays stay on top.
    for (xcb_window_t window : visible_windows)
    {
        auto const* client = get_client(window);
        if (client && client->layer == WindowLayer::Overlay)
        {
            uint32_t values[] = { XCB_STACK_MODE_ABOVE };
            xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, values);
        }
    }

    stacking_dirty_ = true;
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

void WindowManager::send_configure_notify(xcb_window_t window)
{
    auto const* client = get_client(window);
    if (client)
    {
        Geometry geom = (client->kind == Client::Kind::Tiled) ? client->tiled_geometry
            : (client->fullscreen) ? fullscreen_geometry_for_window(window)
            : client->floating_geometry;
        // Fall through to X read if cached geometry is uninitialized (e.g. iconic tiled never laid out)
        if (geom.width > 0 && geom.height > 0)
        {
            send_configure_notify(window, geom, static_cast<uint16_t>(border_width_for_client(*client)));
            return;
        }
    }

    auto geom_cookie = xcb_get_geometry(conn_.get(), window);
    auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
    if (!geom_reply)
        return;

    Geometry geom = { geom_reply->x, geom_reply->y, geom_reply->width, geom_reply->height };
    uint16_t bw = geom_reply->border_width;
    free(geom_reply);
    send_configure_notify(window, geom, bw);
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
    std::string name = get_window_name(window);

    auto* client = get_client(window);
    if (!client)
        return;

    std::string previous_name = client->name;
    client->name = name;

    // Re-evaluate window rules when the title changes, so that title-based
    // rules (e.g. overlay layer) are applied even when the client sets the
    // title after the initial MapRequest (common with Electron apps).
    if (name != previous_name)
    {
        reevaluate_managed_window(window);
        conn_.flush();
    }
}

void WindowManager::update_struts()
{
    for (auto& monitor : monitors_)
    {
        monitor.strut = {};
    }

    for (auto const& [dock, client] : clients_)
    {
        if (client.kind != Client::Kind::Dock)
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
    if (it != clients_.end() && it->second.kind == Client::Kind::Dock)
    {
        clients_.erase(it);
        update_struts();
        rearrange_all_monitors();
        update_ewmh_client_list();
    }
}

void WindowManager::unmanage_desktop_window(xcb_window_t window)
{
    auto it = clients_.find(window);
    if (it != clients_.end() && it->second.kind == Client::Kind::Desktop)
    {
        clients_.erase(it);
        update_ewmh_client_list();
    }
}

void WindowManager::assign_window_workspace(xcb_window_t window, size_t monitor_idx, size_t workspace_idx)
{
    auto* client = get_client(window);
    if (!client)
        return;

    // Transfer fullscreen tracking when monitor changes
    if (client->fullscreen && client->monitor != monitor_idx)
    {
        if (client->monitor < monitors_.size())
            std::erase(monitors_[client->monitor].fullscreen_windows, window);
        if (monitor_idx < monitors_.size())
        {
            auto& fs_list = monitors_[monitor_idx].fullscreen_windows;
            if (std::ranges::find(fs_list, window) == fs_list.end())
                fs_list.push_back(window);
        }
    }

    client->monitor = monitor_idx;
    client->workspace = workspace_idx;
    if (client->sticky)
        ewmh_.set_window_desktop(window, 0xFFFFFFFF);
    else
        ewmh_.set_window_desktop(window, get_ewmh_desktop_index(monitor_idx, workspace_idx));
}

void WindowManager::add_tiled_to_workspace(xcb_window_t window, size_t monitor_idx, size_t workspace_idx)
{
    if (monitor_idx >= monitors_.size() || workspace_idx >= monitors_[monitor_idx].workspaces.size())
    {
        LOG_WARN("add_tiled_to_workspace: invalid indices monitor={} workspace={}", monitor_idx, workspace_idx);
        return;
    }
    monitors_[monitor_idx].workspaces[workspace_idx].windows.push_back(window);
    if (auto* client = get_client(window))
    {
        client->monitor = monitor_idx;
        client->workspace = workspace_idx;
    }
    uint32_t desktop = get_ewmh_desktop_index(monitor_idx, workspace_idx);
    ewmh_.set_window_desktop(window, desktop);
}

void WindowManager::remove_tiled_from_workspace(xcb_window_t window, size_t monitor_idx, size_t workspace_idx)
{
    if (monitor_idx >= monitors_.size() || workspace_idx >= monitors_[monitor_idx].workspaces.size())
    {
        LOG_WARN("remove_tiled_from_workspace: invalid indices monitor={} workspace={}", monitor_idx, workspace_idx);
        return;
    }
    auto is_iconic = [this](xcb_window_t w) { return is_client_iconic(w); };
    workspace_policy::remove_tiled_window(monitors_[monitor_idx].workspaces[workspace_idx], window, is_iconic);
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
}

void WindowManager::flush_and_drain_crossing()
{
    conn_.flush();

    // Round-trip sync: ensures the server has processed all our requests
    // and generated all resulting events into the connection buffer.
    auto cookie = xcb_get_input_focus(conn_.get());
    free(xcb_get_input_focus_reply(conn_.get(), cookie, nullptr));

    // Drain crossing events generated by our visibility changes (moving
    // windows on/off-screen).  Without this, the next event-loop iteration
    // would see EnterNotify/MotionNotify for whichever window now sits under
    // the cursor, overriding the focus we just set via focus_or_fallback().
    while (auto* event = xcb_poll_for_event(conn_.get()))
    {
        uint8_t type = event->response_type & ~0x80;
        if (type == XCB_ENTER_NOTIFY || type == XCB_LEAVE_NOTIFY || type == XCB_MOTION_NOTIFY)
        {
            free(event);
            continue;
        }
        std::unique_ptr<xcb_generic_event_t, decltype(&free)> eventPtr(event, free);
        handle_event(*event);
    }
}

void WindowManager::cache_focus_hints(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;

    // Read WM_HINTS directly from X (the cached default would be stale).
    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), window), &hints, nullptr))
        client->accepts_input = !(hints.flags & XCB_ICCCM_WM_HINT_INPUT) || hints.input;
    else
        client->accepts_input = true; // ICCCM default when WM_HINTS absent

    client->supports_take_focus = supports_protocol(window, wm_take_focus_);
}

void WindowManager::sync_visibility_for_monitor(size_t monitor_idx)
{
    if (monitor_idx >= monitors_.size())
    {
        LOG_WARN("sync_visibility_for_monitor: invalid monitor_idx {}", monitor_idx);
        return;
    }

    for (auto& [id, client] : clients_)
    {
        if (client.kind != Client::Kind::Tiled && client.kind != Client::Kind::Floating)
            continue;
        if (client.monitor != monitor_idx)
            continue;

        bool suppressed = is_suppressed_by_fullscreen(id);
        bool should_be_visible = visibility_policy::is_window_visible(
            showing_desktop_, client.iconic, client.sticky, client.monitor, client.workspace, monitors_
        ) && !suppressed;

        if (should_be_visible && client.hidden)
        {
            show_window(id);
            if (client.kind == Client::Kind::Floating)
            {
                if (client.fullscreen)
                    apply_fullscreen_if_needed(id);
                else if (client.maximized_horz || client.maximized_vert)
                    apply_maximized_geometry(id);
                else
                    apply_floating_geometry(id);

                if (client.transient_for != XCB_NONE)
                    restack_transients(client.transient_for);
            }
            // Tiled geometry is handled by rearrange_monitor / layout_.arrange
        }
        else if (!should_be_visible && !client.hidden)
        {
            hide_window(id);
        }
    }

    // Mark stacking dirty so the EWMH property gets updated on the next
    // event-loop iteration.  Callers that need actual X restacking must call
    // restack_monitor_layers explicitly (or rely on a following rearrange_monitor
    // which already restacks).
    stacking_dirty_ = true;
}

void WindowManager::sync_visibility_all()
{
    for (size_t i = 0; i < monitors_.size(); ++i)
        sync_visibility_for_monitor(i);
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
        if (client.kind == Client::Kind::Floating)
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
