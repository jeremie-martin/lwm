#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/events.hpp"
#include "lwm/core/ewmh.hpp"
#include "lwm/core/floating.hpp"
#include "lwm/core/ipc_server.hpp"
#include "lwm/core/signals.hpp"
#include "lwm/core/state.hpp"
#include "lwm/core/window_rules.hpp"
#include "lwm/layout/layout.hpp"
#include <array>
#include <chrono>
#include <xcb/sync.h>
#include <deque>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace lwm {

// ButtonPress is exclusive: selecting it alongside an application's mask can
// reject the whole ChangeWindowAttributes request. Use passive client grabs for
// clicks; ReplayPointer skips ancestor grabs, so root grabs alone are insufficient.
// PointerMotion is shared and lets focus-following recover when an application
// selected motion events before another window took focus.
constexpr uint32_t kManagedWindowEventMask =
    XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_POINTER_MOTION;

enum class RunResult
{
    Exit,
    Restart,
    Failed
};

// The imperative shell around State: it translates X input and IPC requests
// into State operations, and completion projects State onto the X server.
class WindowManager
{
public:
    WindowManager(Config config, SignalPipe& signals, std::string config_path);
    ~WindowManager();

    RunResult run();
    std::string const& restart_binary() const { return restart_binary_; }
    void prepare_restart();

private:
    // Atoms outside xcb-ewmh's set, interned once at startup.
    struct Atoms
    {
        xcb_atom_t wm_state;
        xcb_atom_t wm_change_state;
        xcb_atom_t wm_delete_window;
        xcb_atom_t wm_take_focus;
        xcb_atom_t wm_s0;
        xcb_atom_t net_wm_state_focused;
        xcb_atom_t lwm_ipc_socket;
        xcb_atom_t lwm_window_class;
        xcb_atom_t lwm_restart;
        xcb_atom_t lwm_restart_owner;
    };

    // Last published values; forget an externally changed field to reconcile it.
    // Domain decisions must use State, not this output cache.
    struct Output
    {
        bool mapped = false;
        bool hidden = false;
        std::optional<Geometry> geometry; ///< Last on-screen rectangle; border width is tracked separately
        uint32_t border_width = 0;
        std::optional<uint32_t> border_color;
        std::optional<uint32_t> wm_state;
        std::optional<uint32_t> desktop;
        std::optional<uint32_t> states; ///< Owned _NET_WM_STATE atoms as bits
        char const* window_class = nullptr;
        std::optional<bool> urgent;
        std::optional<std::optional<FullscreenMonitors>> fullscreen_monitors;
        // Protocol bookkeeping, not projections of state.
        bool ignore_urgency_echo = false;
        uint32_t sync_counter = 0;
        uint64_t sync_value = 0;
    };

    struct DesktopLayout
    {
        uint32_t count = 0;
        std::vector<std::string> names;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<std::pair<uint32_t, uint32_t>> viewports;
        std::vector<Geometry> workareas;
        bool operator==(DesktopLayout const&) const = default;
    };

    struct RootOutput
    {
        // Unset until first written, so a fresh WM replaces stale lists even when empty.
        std::optional<std::vector<xcb_window_t>> client_list;
        std::optional<std::vector<xcb_window_t>> stacking;
        std::optional<xcb_window_t> active;
        std::optional<uint32_t> current_desktop;
        std::optional<bool> showing_desktop;
        std::optional<DesktopLayout> desktops;
        std::vector<xcb_window_t> fullscreen_owners; ///< Logged ownership per monitor
        std::map<std::string, size_t> workspaces; ///< Current workspace per output name
        std::string snapshot;                     ///< State payload behind the last state_change
        uint64_t snapshot_revision = UINT64_MAX;  ///< Revision the snapshot was taken at
        uint64_t subscriptions = 0;               ///< Subscriptions seen when the snapshot was taken
    };

    struct WindowDrag
    {
        xcb_window_t window;
        Client::Kind kind;
        size_t monitor;
        size_t workspace;
        Geometry start_geometry;
        floating::ResizeEdge edges;
    };

    struct TiledResize
    {
        size_t monitor;
        size_t workspace;
        SplitHitResult split;
        Geometry area;
        LayoutStrategy strategy;
        std::vector<xcb_window_t> participants;
    };

    struct Drag
    {
        std::variant<WindowDrag, TiledResize> operation;
        int16_t start_x, start_y;
        int16_t last_x, last_y;
        uint8_t button; // Zero accepts any release (unspecified EWMH button).
    };

    using StateUpdates = std::vector<std::pair<xcb_window_t, std::vector<xcb_atom_t>>>;

    struct SplitBorderHit
    {
        SplitHitResult hit;
        size_t monitor;
    };

    Connection conn_;
    Ewmh ewmh_;
    Atoms atoms_{ };
    State state_;
    std::unordered_map<xcb_window_t, Output> outputs_;
    RootOutput root_;
    ipc::Server ipc_;
    // Process-owned signal handlers and reload pipe survive WM reconstruction.
    SignalPipe& signals_;
    std::string config_path_;
    xcb_window_t wm_window_ = XCB_NONE;
    std::optional<restart::Snapshot> handoff_; ///< Predecessor state, consumed during startup adoption

    // Obligations of the current operation that are not projections of state.
    std::set<xcb_window_t> configure_replies_;
    std::vector<Event> events_;
    bool restack_requested_ = false;
    bool drain_requested_ = false;
    bool monitors_dirty_ = false;
    // Projection inputs outside State (drag preview, configuration, outputs
    // forgotten after an external change). State changes are tracked by revision.
    bool presentation_dirty_ = false;
    uint64_t published_revision_ = UINT64_MAX;

    std::deque<xcb_generic_event_t> deferred_events_;
    std::unordered_map<xcb_window_t, std::chrono::steady_clock::time_point> pending_kills_;
    std::optional<Drag> drag_;
    bool running_ = true;
    bool restarting_ = false;
    std::string restart_binary_;
    uint32_t last_event_time_ = XCB_CURRENT_TIME;
    // Latest timestamp from an actual input event (key/button/motion/crossing).
    // Unlike last_event_time_, never fed by PropertyNotify: user_time stamping
    // must reflect user interaction, not property churn.
    uint32_t last_input_time_ = XCB_CURRENT_TIME;
    xcb_keysym_t last_toggle_keysym_ = XCB_NO_SYMBOL;
    xcb_timestamp_t last_toggle_release_time_ = 0;
    xcb_cursor_t cursor_default_ = XCB_NONE;
    xcb_cursor_t cursor_resize_h_ = XCB_NONE;
    xcb_cursor_t cursor_resize_v_ = XCB_NONE;
    xcb_cursor_t current_root_cursor_ = XCB_NONE;
    // Double-click detection for split ratio reset
    xcb_timestamp_t last_gap_click_time_ = 0;
    SplitAddress last_gap_click_address_{ };
    size_t last_gap_click_monitor_ = 0;

    // wm.cpp: process, X setup, and X reads
    void intern_atoms();
    void create_wm_window();
    void create_cursors();
    void setup_root();
    void claim_wm_ownership();
    bool release_predecessor();
    void grab_buttons();
    void grab_keys();
    Config const& config() const { return state_.config(); }
    Topology discover_topology();
    void refresh_topology();
    void setup_ipc();
    void cleanup_ipc();
    void dispatch_event(xcb_generic_event_t const& event, size_t& remaining, std::chrono::steady_clock::time_point deadline);
    std::expected<void, std::string> reload_config();
    void report_reload(std::expected<void, std::string> const& result, std::string_view source);
    bool launch_program(std::vector<std::string> const& command, std::string_view source);
    void kill_window(xcb_window_t window);
    void handle_timeouts();
    std::optional<Geometry> read_window_geometry(xcb_window_t window) const;
    std::string read_window_name(xcb_window_t window) const;
    std::pair<std::string, std::string> read_wm_class(xcb_window_t window) const;
    std::optional<uint32_t> read_window_desktop(xcb_window_t window) const;
    std::optional<xcb_window_t> read_transient_for(xcb_window_t window) const;
    uint32_t read_user_time(xcb_window_t window, xcb_window_t time_window) const;
    xcb_window_t read_user_time_window(xcb_window_t window);
    std::optional<FullscreenMonitors> read_fullscreen_monitors(xcb_window_t window) const;
    bool is_override_redirect(xcb_window_t window) const;
    bool supports_protocol(xcb_window_t window, xcb_atom_t protocol) const;
    void send_protocol_message(xcb_window_t window, xcb_atom_t protocol, uint32_t timestamp, uint32_t d2 = 0, uint32_t d3 = 0);
    void read_sync_counter(xcb_window_t window);
    void set_root_cursor(xcb_cursor_t cursor);

    // wm_manage.cpp: classification, registration, rules
    void scan_existing_windows(bool handoff);
    void manage_fixture(xcb_window_t window, Fixture::Role role, bool adopting);
    // Startup receives deferred observations; live admission registers immediately.
    std::optional<Client> admit_window(xcb_window_t window, bool adopting);
    void apply_size_hints(xcb_window_t window, bool initial);
    struct SizeHints
    {
        std::optional<std::pair<int16_t, int16_t>> position;
        std::optional<uint16_t> width;
        std::optional<uint16_t> height;
    };
    SizeHints read_size_hints(xcb_window_t window, bool anchored) const;
    void place_new_client(xcb_window_t window);
    std::optional<Geometry> placement_parent_geometry(xcb_window_t window) const;
    void follow_floating_geometry(xcb_window_t window);

    // wm_transition.cpp: operation completion and publication
    void complete_transition();
    Geometry presentation_geometry(Client const& client) const;
    std::optional<Geometry> drag_preview(Client const& client) const;
    uint32_t border_width(Client const& client) const;
    uint32_t border_color(Client const& client) const;
    bool publish_clients(std::vector<State::Projected> const& clients);
    bool write_geometry(Client const& client, Output& output, Geometry geometry, uint32_t border);
    void send_configure_notify(xcb_window_t window, Geometry geometry, uint32_t border);
    bool publish_properties(Client const& client, Output& output, StateUpdates& states);
    std::array<xcb_atom_t, 12> owned_state_atoms() const;
    void publish_window_class(xcb_window_t window, char const* kind);
    void publish_urgency(Client const& client, Output& output);
    void publish_fixtures();
    void publish_root(std::vector<State::Projected> const& clients, bool urgency_changed);
    DesktopLayout desktop_layout() const;
    void reconcile_stacking(State::FullscreenVisibility const& fullscreen, bool reassert);
    void withdraw_removed();
    void commit_focus(uint32_t time);
    void flush_and_drain_crossing();
    void emit_events(bool focus_requested);
    void queue_event(Event event);
    uint32_t desktop_index(size_t monitor, size_t workspace) const;

    // wm_events.cpp: X event handlers
    void handle_event(xcb_generic_event_t const& event);
    void handle_map_request(xcb_map_request_event_t const& e);
    void handle_window_removal(xcb_window_t window);
    void handle_enter_notify(xcb_enter_notify_event_t const& e);
    void handle_motion_notify(xcb_motion_notify_event_t const& e);
    void handle_button_press(xcb_button_press_event_t const& e);
    void handle_button_release(xcb_button_release_event_t const& e);
    void handle_key_press(xcb_key_press_event_t const& e);
    void handle_key_release(xcb_key_release_event_t const& e);
    bool is_auto_repeat_toggle(xcb_keysym_t keysym, xcb_timestamp_t time);
    void handle_client_message(xcb_client_message_event_t const& e);
    void handle_restack_message(xcb_client_message_event_t const& e);
    void handle_wm_state_change(xcb_client_message_event_t const& e);
    void handle_active_window_request(xcb_client_message_event_t const& e);
    void handle_desktop_change(xcb_client_message_event_t const& e);
    void handle_moveresize_window(xcb_client_message_event_t const& e);
    void handle_wm_moveresize(xcb_client_message_event_t const& e);
    void handle_showing_desktop(xcb_client_message_event_t const& e);
    void handle_configure_request(xcb_configure_request_event_t const& e);
    void handle_property_notify(xcb_property_notify_event_t const& e);
    void handle_wm_hints(Client const& client);
    void update_floating_geometry(Client const& client, Geometry geometry);
    MousebindConfig const* resolve_mouse_binding(uint16_t state, uint8_t button) const;

    // wm_actions.cpp: the one executor for key bindings and IPC
    std::expected<std::string, std::string> execute(Action const& action, std::string_view source);
    void toggle_float(xcb_window_t window);
    void switch_to_desktop(uint32_t desktop);
    void focus_adjacent_monitor(int direction);
    void move_active_to_monitor(int direction);
    size_t wrap_monitor(int index) const;
    void warp_to_monitor(Monitor const& monitor);
    void swap_active_tile(int offset);
    void layout_changed(
        Action const& action,
        std::optional<event::LayoutValue> value = {},
        std::optional<double> delta = {}
    );

    // wm_events.cpp: pointer-driven monitor selection
    void focus_monitor_at_point(int16_t x, int16_t y);

    // wm_drag.cpp
    bool drag_active() const { return drag_.has_value(); }
    bool grab_pointer_for_drag(xcb_cursor_t cursor = XCB_NONE);
    void begin_window_drag(xcb_window_t window, int16_t x, int16_t y, uint8_t button, floating::ResizeEdge edges = floating::ResizeEdge::None);
    void begin_tiled_resize(SplitHitResult const& hit, size_t monitor, int16_t x, int16_t y, uint8_t button);
    void update_drag(int16_t x, int16_t y);
    void end_drag(bool commit = true);
    void validate_drag();
    std::optional<SplitBorderHit> hit_split_border(int16_t x, int16_t y) const;

    // wm_scratchpad.cpp
    std::expected<void, std::string> toggle_scratchpad(std::string_view name);
    void stash_window(xcb_window_t window);
    void cycle_scratchpad_pool();
    void show_pooled_scratchpad(xcb_window_t window);

    // wm_ipc.cpp
    std::string handle_request(command::Request const& request);
    std::string workspace_list_json() const;
    std::string window_list_json() const;
    std::string scratchpad_list_json() const;
    std::string state_json() const;

    // wm_restart.cpp
    void read_handoff();
};

} // namespace lwm
