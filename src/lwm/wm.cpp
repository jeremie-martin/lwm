#include "wm.hpp"
#include "lwm/core/invariants.hpp"
#include "lwm/core/ipc.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/xproperty.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>
#include <xcb/xcb_icccm.h>

namespace lwm {

namespace {

constexpr auto KILL_TIMEOUT = std::chrono::seconds(5);

template <typename T> using Reply = std::unique_ptr<T, decltype(&free)>;
template <typename T> Reply<T> reply(T* value) { return { value, &free }; }

} // namespace

WindowManager::WindowManager(Config config, SignalPipe& signals, std::string config_path)
    : config_(std::move(config))
    , ewmh_(conn_)
    , signals_(signals)
    , config_path_(std::move(config_path))
{
    state_.configure_layout(config_.appearance, config_.layout);
    intern_atoms();
    create_wm_window();
    setup_root();
    claim_wm_ownership();
    bool handoff = release_predecessor();
    create_cursors();
    grab_buttons();
    configure_scratchpads();
    refresh_topology();
    ewmh_.init_atoms({ ewmh_.get()->_NET_WM_USER_TIME_WINDOW, atoms_.net_wm_state_focused, atoms_.lwm_window_class });
    ewmh_.set_wm_name("lwm");
    setup_ipc();
    scan_existing_windows(handoff);
    grab_keys();
    complete_transition();
    LWM_LOG_INFO(
        "WM ready: monitors={} clients={} restart={} active_window={:#x}",
        state_.monitors().size(),
        state_.clients().size(),
        handoff,
        state_.active_window()
    );
}

WindowManager::~WindowManager() { cleanup_ipc(); }

// X setup

void WindowManager::intern_atoms()
{
    std::pair<char const*, xcb_atom_t*> const names[] = {
        { "WM_STATE", &atoms_.wm_state },
        { "WM_CHANGE_STATE", &atoms_.wm_change_state },
        { "WM_DELETE_WINDOW", &atoms_.wm_delete_window },
        { "WM_TAKE_FOCUS", &atoms_.wm_take_focus },
        { "WM_S0", &atoms_.wm_s0 },
        { "_NET_WM_STATE_FOCUSED", &atoms_.net_wm_state_focused },
        { "_LWM_IPC_SOCKET", &atoms_.lwm_ipc_socket },
        { "_LWM_WINDOW_CLASS", &atoms_.lwm_window_class },
        { "_LWM_RESTART", &atoms_.lwm_restart },
        { "_LWM_RESTART_OWNER", &atoms_.lwm_restart_owner },
    };
    std::vector<xcb_intern_atom_cookie_t> cookies;
    for (auto const& [name, atom] : names)
        cookies.push_back(xcb_intern_atom(conn_.get(), 0, static_cast<uint16_t>(std::strlen(name)), name));
    for (size_t i = 0; i < cookies.size(); ++i)
    {
        auto atom = reply(xcb_intern_atom_reply(conn_.get(), cookies[i], nullptr));
        if (!atom)
            throw std::runtime_error(std::string("Failed to intern ") + names[i].first);
        *names[i].second = atom->atom;
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

void WindowManager::create_cursors()
{
    xcb_font_t font = xcb_generate_id(conn_.get());
    xcb_open_font(conn_.get(), font, 6, "cursor");
    auto glyph = [&](uint16_t shape)
    {
        xcb_cursor_t cursor = xcb_generate_id(conn_.get());
        xcb_create_glyph_cursor(conn_.get(), cursor, font, font, shape, shape + 1, 0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF);
        return cursor;
    };
    cursor_default_ = glyph(68);   // left_ptr
    cursor_resize_h_ = glyph(108); // sb_h_double_arrow
    cursor_resize_v_ = glyph(116); // sb_v_double_arrow
    xcb_close_font(conn_.get(), font);
    set_root_cursor(cursor_default_);
}

void WindowManager::set_root_cursor(xcb_cursor_t cursor)
{
    if (cursor == current_root_cursor_)
        return;
    xcb_change_window_attributes(conn_.get(), conn_.screen()->root, XCB_CW_CURSOR, &cursor);
    current_root_cursor_ = cursor;
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
        xcb_randr_select_input(
            conn_.get(),
            conn_.screen()->root,
            XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE
        );
}

void WindowManager::claim_wm_ownership()
{
    auto owner = [&] { return reply(xcb_get_selection_owner_reply(conn_.get(), xcb_get_selection_owner(conn_.get(), atoms_.wm_s0), nullptr)); };
    auto current = owner();
    if (!current)
        throw std::runtime_error("Failed to query WM selection owner");
    if (current->owner != XCB_NONE)
        throw std::runtime_error("Another window manager already owns WM_S0");
    xcb_set_selection_owner(conn_.get(), wm_window_, atoms_.wm_s0, XCB_CURRENT_TIME);
    current = owner();
    if (!current || current->owner != wm_window_)
        throw std::runtime_error("Failed to acquire WM_S0 selection");

    xcb_client_message_event_t event{ };
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = conn_.screen()->root;
    event.type = ewmh_.get()->MANAGER;
    event.data.data32[0] = XCB_CURRENT_TIME;
    event.data.data32[1] = atoms_.wm_s0;
    event.data.data32[2] = wm_window_;
    xcb_send_event(conn_.get(), 0, conn_.screen()->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<char const*>(&event));
}

// Root ownership is established. Release a marked predecessor only now, after
// opening our connection, so an otherwise empty X server cannot reset. Returns
// whether this start continues an exec restart.
bool WindowManager::release_predecessor()
{
    auto root = conn_.screen()->root;
    auto predecessor = xproperty::scalar(conn_.get(), root, atoms_.lwm_restart_owner, XCB_ATOM_WINDOW);
    bool handoff = predecessor && *predecessor != XCB_NONE && *predecessor != wm_window_
        && xproperty::scalar(conn_.get(), *predecessor, atoms_.lwm_restart_owner, XCB_ATOM_WINDOW) == predecessor;
    if (handoff)
        xcb_kill_client(conn_.get(), *predecessor);
    xcb_delete_property(conn_.get(), root, atoms_.lwm_restart_owner);
    read_handoff();
    return handoff;
}

void WindowManager::grab_buttons()
{
    xcb_window_t root = conn_.screen()->root;
    xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, root, XCB_MOD_MASK_ANY);
    for (auto const& binding : config_.mousebinds)
        for (uint16_t lock : kIgnoredModifierCombinations)
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
                binding.modifier | lock
            );
}

// Root grabs take precedence over any client grab, so bindings need no per-window grabs.
void WindowManager::grab_keys()
{
    xcb_window_t root = conn_.screen()->root;
    xcb_ungrab_key(conn_.get(), XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);
    for (auto const& [binding, action] : config_.keybinds)
    {
        auto keycodes = reply(xcb_key_symbols_get_keycode(conn_.keysyms(), binding.keysym));
        for (auto* keycode = keycodes.get(); keycode && *keycode != XCB_NO_SYMBOL; ++keycode)
            for (uint16_t lock : kIgnoredModifierCombinations)
                xcb_grab_key(conn_.get(), 1, root, binding.modifier | lock, *keycode, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
}

void WindowManager::setup_ipc()
{
    ipc_.start(ipc::default_socket_path().string());
    ipc::set_root_text_property(conn_.get(), conn_.screen()->root, atoms_.lwm_ipc_socket, ewmh_.get()->UTF8_STRING, ipc_.path());
    conn_.flush();
}

void WindowManager::cleanup_ipc()
{
    ipc_.stop();
    if (conn_.get() && !xcb_connection_has_error(conn_.get()))
    {
        ipc::delete_root_property(conn_.get(), conn_.screen()->root, atoms_.lwm_ipc_socket);
        conn_.flush();
    }
}

// Monitors and workareas

std::vector<Monitor> WindowManager::discover_monitors()
{
    std::vector<Monitor> discovered;
    auto add = [&](std::string name, xcb_randr_output_t output, Geometry geometry)
    {
        Monitor monitor;
        monitor.output = output;
        monitor.name = std::move(name);
        monitor.geometry = geometry;
        Workspace workspace;
        workspace.layout_strategy = config_.layout.strategy;
        monitor.workspaces.assign(config_.workspaces.count, workspace);
        discovered.push_back(std::move(monitor));
    };
    if (conn_.has_randr())
    {
        auto resources = reply(xcb_randr_get_screen_resources_current_reply(
            conn_.get(),
            xcb_randr_get_screen_resources_current(conn_.get(), conn_.screen()->root),
            nullptr
        ));
        if (!resources)
            LWM_LOG_WARN_LIMIT(std::chrono::seconds(5), "randr: screen resources unavailable; using one monitor");
        int count = resources ? xcb_randr_get_screen_resources_current_outputs_length(resources.get()) : 0;
        auto* outputs = resources ? xcb_randr_get_screen_resources_current_outputs(resources.get()) : nullptr;
        for (int i = 0; i < count; ++i)
        {
            auto output = reply(xcb_randr_get_output_info_reply(
                conn_.get(),
                xcb_randr_get_output_info(conn_.get(), outputs[i], resources->config_timestamp),
                nullptr
            ));
            if (!output)
            {
                LWM_LOG_WARN_LIMIT(std::chrono::seconds(5), "randr: output {:#x} unavailable, skipping", outputs[i]);
                continue;
            }
            if (output->connection != XCB_RANDR_CONNECTION_CONNECTED || output->crtc == XCB_NONE)
                continue;
            std::string name(
                reinterpret_cast<char*>(xcb_randr_get_output_info_name(output.get())),
                xcb_randr_get_output_info_name_length(output.get())
            );
            auto crtc = reply(xcb_randr_get_crtc_info_reply(
                conn_.get(),
                xcb_randr_get_crtc_info(conn_.get(), output->crtc, resources->config_timestamp),
                nullptr
            ));
            if (!crtc || crtc->width == 0 || crtc->height == 0)
            {
                LWM_LOG_WARN_LIMIT(std::chrono::seconds(5), "randr: output {} has no usable CRTC, skipping", name);
                continue;
            }
            add(std::move(name), outputs[i], { crtc->x, crtc->y, crtc->width, crtc->height });
        }
    }
    if (discovered.empty())
        add("default", XCB_NONE, { 0, 0, conn_.screen()->width_in_pixels, conn_.screen()->height_in_pixels });
    std::ranges::sort(discovered, { }, [](Monitor const& monitor) { return monitor.geometry.x; });
    return discovered;
}

// Discovery and dock reservations are read before the model changes, so
// rebinding and floating fit use the new workareas in one state operation.
void WindowManager::refresh_topology()
{
    end_drag(false);
    auto monitors = discover_monitors();
    auto struts = dock_struts(monitors);
    for (size_t i = 0; i < monitors.size(); ++i) monitors[i].strut = struts[i];
    state_.replace_monitors(std::move(monitors));
    workareas_dirty_ = false;
}

void WindowManager::refresh_workareas()
{
    auto struts = dock_struts(state_.monitors());
    for (size_t i = 0; i < struts.size(); ++i) state_.workarea(i, struts[i]);
}

// Reads current dock properties and, when a reservation exists, the root
// geometry once. There is no persistent dock-property cache.
std::vector<Strut> WindowManager::dock_struts(std::span<Monitor const> monitors)
{
    std::vector<Strut> struts(monitors.size());
    std::optional<Geometry> root;
    for (auto const& [id, fixture] : state_.fixtures())
    {
        if (fixture.role != Fixture::Role::Dock)
            continue;
        auto reservation = ewmh_.get_window_strut(id);
        if (reservation.empty())
            continue;
        if (!root)
        {
            auto geometry = read_window_geometry(conn_.screen()->root);
            if (!geometry)
                break;
            root = Geometry{ 0, 0, geometry->width, geometry->height };
        }
        for (size_t i = 0; i < monitors.size(); ++i)
        {
            auto strut = monitor_strut(reservation, *root, monitors[i].geometry);
            struts[i].left = std::max(struts[i].left, strut.left);
            struts[i].right = std::max(struts[i].right, strut.right);
            struts[i].top = std::max(struts[i].top, strut.top);
            struts[i].bottom = std::max(struts[i].bottom, strut.bottom);
        }
    }
    return struts;
}

// Event loop

RunResult WindowManager::run()
{
    LWM_ASSERT_INVARIANTS(state_);
    int xfd = xcb_get_file_descriptor(conn_.get());
    constexpr size_t POLL_SIGNAL = 1;
    constexpr size_t POLL_IPC = 2;
    std::vector<pollfd> poll_fds;

    while (running_)
    {
        std::optional<std::chrono::steady_clock::time_point> deadline = ipc_.deadline();
        for (auto const& [window, kill_at] : pending_kills_)
            if (!deadline || kill_at < *deadline)
                deadline = kill_at;
        int timeout_ms = -1;
        if (deadline)
            timeout_ms = static_cast<int>(std::max<int64_t>(
                0,
                std::chrono::ceil<std::chrono::milliseconds>(*deadline - std::chrono::steady_clock::now()).count()
            ));
        // Replies can pull events into XCB's queue without leaving the fd readable.
        while (auto* event = xcb_poll_for_queued_event(conn_.get()))
        {
            deferred_events_.push_back(*event);
            free(event);
        }
        if (!deferred_events_.empty())
            timeout_ms = 0;

        poll_fds.clear();
        poll_fds.push_back({ .fd = xfd, .events = POLLIN, .revents = 0 });
        poll_fds.push_back({ .fd = signals_.fd(), .events = POLLIN, .revents = 0 });
        ipc_.append_poll_fds(poll_fds);
        if (poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), timeout_ms) > 0)
        {
            if (poll_fds[POLL_SIGNAL].revents & POLLIN)
            {
                signals_.drain();
                report_reload(reload_config(), "sighup");
                complete_transition();
            }
            ipc_.dispatch(
                std::span(poll_fds).subspan(POLL_IPC),
                [this](command::Request const& request)
                {
                    auto response = handle_request(request);
                    complete_transition();
                    return response;
                }
            );
            // A new subscriber compares later changes with the state it could query now.
            if (std::exchange(root_.subscriptions, ipc_.subscriptions()) != ipc_.subscriptions())
            {
                root_.snapshot = state_json();
                root_.snapshot_revision = state_.revision();
            }
        }

        // Bounded batches keep IPC, signals and deadlines responsive under X load.
        size_t remaining = 64;
        auto batch_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        while (remaining && std::chrono::steady_clock::now() < batch_deadline)
        {
            if (!deferred_events_.empty())
            {
                auto event = deferred_events_.front();
                deferred_events_.pop_front();
                --remaining;
                dispatch_event(event, remaining, batch_deadline);
            }
            else if (auto* event = xcb_poll_for_event(conn_.get()))
            {
                Reply<xcb_generic_event_t> owned(event, &free);
                --remaining;
                dispatch_event(*owned, remaining, batch_deadline);
            }
            else
                break;
        }

        ipc_.expire();
        handle_timeouts();
        if (std::exchange(monitors_dirty_, false))
        {
            refresh_topology();
            state_.focus_fallback(state_.focused_monitor());
            drain_requested_ = true;
        }
        complete_transition();
        // Flush direct protocol replies even when no managed transition was needed.
        conn_.flush();

        if (int error = xcb_connection_has_error(conn_.get()))
        {
            LWM_LOG_CRITICAL("X connection failed: code={}; shutting down", error);
            return RunResult::Failed;
        }
    }
    return restarting_ ? RunResult::Restart : RunResult::Exit;
}

void WindowManager::dispatch_event(
    xcb_generic_event_t const& event,
    size_t& remaining,
    std::chrono::steady_clock::time_point deadline
)
{
    // Compress queued drag motion up to the first non-motion event.
    auto current = event;
    if ((event.response_type & ~0x80) == XCB_MOTION_NOTIFY && drag_active())
    {
        while (remaining && std::chrono::steady_clock::now() < deadline)
        {
            if (deferred_events_.empty())
            {
                auto* next = xcb_poll_for_queued_event(conn_.get());
                if (!next)
                    break;
                deferred_events_.push_back(*next);
                free(next);
            }
            if ((deferred_events_.front().response_type & ~0x80) != XCB_MOTION_NOTIFY)
                break;
            --remaining;
            current = deferred_events_.front();
            deferred_events_.pop_front();
        }
    }
    handle_event(current);
    complete_transition();
}

void WindowManager::handle_timeouts()
{
    auto now = std::chrono::steady_clock::now();
    std::erase_if(
        pending_kills_,
        [&](auto const& entry)
        {
            if (entry.second > now)
                return false;
            LWM_LOG_WARN("Close timed out: window={:#x}; killing client connection", entry.first);
            xcb_kill_client(conn_.get(), entry.first);
            return true;
        }
    );
}

// Configuration

// A candidate is validated before anything changes; an invalid file leaves
// the active configuration and runtime claims untouched.
std::expected<void, std::string> WindowManager::reload_config()
{
    if (config_path_.empty())
        return std::unexpected("no config path is configured");
    if (!std::filesystem::exists(config_path_))
        return std::unexpected("config file does not exist: " + config_path_);
    auto loaded = load_config_result(config_path_);
    if (!loaded)
        return std::unexpected(loaded.error());
    if (loaded->workspaces.count != config_.workspaces.count)
        return std::unexpected("live reload of [workspaces].count is unsupported; restart required");

    end_drag(false);
    config_ = std::move(*loaded);
    state_.configure_layout(config_.appearance, config_.layout);
    configure_scratchpads();
    grab_buttons();
    grab_keys();
    reapply_rules();
    state_.request_focus_repair();
    return { };
}

void WindowManager::report_reload(std::expected<void, std::string> const& result, std::string_view source)
{
    if (result)
        LWM_LOG_INFO("Config reloaded successfully ({})", source);
    else
        LWM_LOG_WARN_LIMIT(std::chrono::seconds(5), "Config reload failed ({}): {}", source, result.error());
    queue_event(event::config_reload{ result.has_value(), source, result ? std::nullopt : std::optional{ result.error() } });
}

// Processes and window lifetime

bool WindowManager::launch_program(std::vector<std::string> const& command, std::string_view source)
{
    if (command.empty())
        return false;
    std::vector<char*> argv;
    for (auto const& arg : command) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    // posix_spawn reports exec failure to the parent. Owned WM descriptors are
    // CLOEXEC; stderr is inherited so the application can report its own errors.
    posix_spawnattr_t attributes;
    char const* stage = "attributes";
    int error = posix_spawnattr_init(&attributes);
    if (!error)
    {
        stage = "session";
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
        pid_t child;
        if (!error)
        {
            stage = "spawn";
            error = posix_spawnp(&child, argv.front(), nullptr, &attributes, argv.data(), environ);
        }
        posix_spawnattr_destroy(&attributes);
    }
    if (error)
        LWM_LOG_ERROR(
            "Launch failed: source={} executable={} stage={} code={} error={}",
            source,
            argv.front(),
            stage,
            error,
            std::strerror(error)
        );
    else
        LWM_LOG_DEBUG("Launched: source={} executable={}", source, argv.front());
    return error == 0;
}

// A ping reply cancels the pending force-kill even if the client stays open
// (for example, to show a save dialog). See X11.md for close behavior.
void WindowManager::kill_window(xcb_window_t window)
{
    if (!supports_protocol(window, atoms_.wm_delete_window))
    {
        LWM_LOG_DEBUG("Close: window={:#x} has no WM_DELETE_WINDOW; killing client connection", window);
        xcb_kill_client(conn_.get(), window);
        return;
    }
    send_protocol_message(window, atoms_.wm_delete_window, last_event_time_);
    if (supports_protocol(window, ewmh_.get()->_NET_WM_PING))
        send_protocol_message(window, ewmh_.get()->_NET_WM_PING, last_event_time_, window);
    LWM_LOG_DEBUG("Close requested: window={:#x} protocol=WM_DELETE_WINDOW", window);
    pending_kills_[window] = std::chrono::steady_clock::now() + KILL_TIMEOUT;
}

// X reads and protocol messages

std::optional<Geometry> WindowManager::read_window_geometry(xcb_window_t window) const
{
    auto geometry = reply(xcb_get_geometry_reply(conn_.get(), xcb_get_geometry(conn_.get(), window), nullptr));
    if (!geometry)
        return std::nullopt;
    return Geometry{ geometry->x, geometry->y, geometry->width, geometry->height };
}

std::string WindowManager::read_window_name(xcb_window_t window) const
{
    if (auto name = xproperty::text_prefix(conn_.get(), window, ewmh_.get()->_NET_WM_NAME, ewmh_.get()->UTF8_STRING, 1024);
        name && !name->empty())
        return *name;
    if (auto name = xproperty::text_prefix(conn_.get(), window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 1024);
        name && !name->empty())
        return *name;
    return "Unnamed";
}

std::pair<std::string, std::string> WindowManager::read_wm_class(xcb_window_t window) const
{
    xcb_icccm_get_wm_class_reply_t wm_class;
    if (!xcb_icccm_get_wm_class_reply(conn_.get(), xcb_icccm_get_wm_class(conn_.get(), window), &wm_class, nullptr))
        return { "", "" };
    std::pair<std::string, std::string> result{ wm_class.instance_name ? wm_class.instance_name : "",
                                                wm_class.class_name ? wm_class.class_name : "" };
    xcb_icccm_get_wm_class_reply_wipe(&wm_class);
    return result;
}

// Preserves 0xFFFFFFFF (sticky); absence or an unreadable property yields nullopt.
std::optional<uint32_t> WindowManager::read_window_desktop(xcb_window_t window) const
{
    uint32_t desktop = 0;
    if (!xcb_ewmh_get_wm_desktop_reply(ewmh_.get(), xcb_ewmh_get_wm_desktop(ewmh_.get(), window), &desktop, nullptr))
        return std::nullopt;
    return desktop;
}

std::optional<xcb_window_t> WindowManager::read_transient_for(xcb_window_t window) const
{
    auto value = xproperty::scalar(conn_.get(), window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW);
    return value && *value != XCB_NONE ? value : std::nullopt;
}

// EWMH user time lives on _NET_WM_USER_TIME_WINDOW when the client names one.
uint32_t WindowManager::read_user_time(xcb_window_t window, xcb_window_t time_window) const
{
    return xproperty::scalar(
               conn_.get(),
               time_window != XCB_NONE ? time_window : window,
               ewmh_.get()->_NET_WM_USER_TIME,
               XCB_ATOM_CARDINAL
    )
        .value_or(0);
}

xcb_window_t WindowManager::read_user_time_window(xcb_window_t window)
{
    auto time_window = xproperty::scalar(conn_.get(), window, ewmh_.get()->_NET_WM_USER_TIME_WINDOW, XCB_ATOM_WINDOW);
    if (!time_window || *time_window == XCB_NONE)
        return XCB_NONE;
    if (*time_window != window)
    {
        // Observe user-time updates on the separate window without replacing its mask.
        auto attributes = reply(
            xcb_get_window_attributes_reply(conn_.get(), xcb_get_window_attributes(conn_.get(), *time_window), nullptr)
        );
        uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | (attributes ? attributes->your_event_mask : 0);
        xcb_change_window_attributes(conn_.get(), *time_window, XCB_CW_EVENT_MASK, &mask);
    }
    return *time_window;
}

std::optional<FullscreenMonitors> WindowManager::read_fullscreen_monitors(xcb_window_t window) const
{
    xcb_ewmh_get_wm_fullscreen_monitors_reply_t monitors;
    if (!xcb_ewmh_get_wm_fullscreen_monitors_reply(
            ewmh_.get(),
            xcb_ewmh_get_wm_fullscreen_monitors(ewmh_.get(), window),
            &monitors,
            nullptr
        ))
        return std::nullopt;
    return FullscreenMonitors{ monitors.top, monitors.bottom, monitors.left, monitors.right };
}

bool WindowManager::is_override_redirect(xcb_window_t window) const
{
    auto attributes =
        reply(xcb_get_window_attributes_reply(conn_.get(), xcb_get_window_attributes(conn_.get(), window), nullptr));
    return attributes && attributes->override_redirect;
}

bool WindowManager::supports_protocol(xcb_window_t window, xcb_atom_t protocol) const
{
    xcb_icccm_get_wm_protocols_reply_t protocols;
    if (!xcb_icccm_get_wm_protocols_reply(
            conn_.get(),
            xcb_icccm_get_wm_protocols(conn_.get(), window, ewmh_.get()->WM_PROTOCOLS),
            &protocols,
            nullptr
        ))
        return false;
    bool supported = std::ranges::find(protocols.atoms, protocols.atoms + protocols.atoms_len, protocol)
        != protocols.atoms + protocols.atoms_len;
    xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
    return supported;
}

void WindowManager::send_protocol_message(
    xcb_window_t window,
    xcb_atom_t protocol,
    uint32_t timestamp,
    uint32_t d2,
    uint32_t d3
)
{
    xcb_client_message_event_t event{ };
    event.response_type = XCB_CLIENT_MESSAGE;
    event.window = window;
    event.type = ewmh_.get()->WM_PROTOCOLS;
    event.format = 32;
    event.data.data32[0] = protocol;
    event.data.data32[1] = timestamp ? timestamp : XCB_CURRENT_TIME;
    event.data.data32[2] = d2;
    event.data.data32[3] = d3;
    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&event));
}

// Basic and extended sync properties both start with the basic counter.
void WindowManager::read_sync_counter(xcb_window_t window)
{
    auto& output = outputs_[window];
    output.sync_counter = 0;
    output.sync_value = 0;
    if (!supports_protocol(window, ewmh_.get()->_NET_WM_SYNC_REQUEST))
        return;
    auto property = xproperty::read(conn_.get(), window, ewmh_.get()->_NET_WM_SYNC_REQUEST_COUNTER, XCB_ATOM_CARDINAL, 2);
    auto counters = xproperty::words(property, XCB_ATOM_CARDINAL);
    if (counters.empty() || counters.front() == XCB_NONE)
        return;
    output.sync_counter = counters.front();
    if (auto counter = reply(xcb_sync_query_counter_reply(conn_.get(), xcb_sync_query_counter(conn_.get(), output.sync_counter), nullptr)))
        output.sync_value = (static_cast<uint64_t>(counter->counter_value.hi) << 32) | counter->counter_value.lo;
}

} // namespace lwm
