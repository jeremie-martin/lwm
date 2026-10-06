#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/ewmh.hpp"
#include "lwm/core/command.hpp"
#include "lwm/core/state.hpp"
#include <array>
#include <chrono>
#include <deque>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace lwm {

// ButtonPress is exclusive: selecting it alongside an application's mask can
// reject the whole ChangeWindowAttributes request. Use passive client grabs for
// clicks; ReplayPointer skips ancestor grabs, so root grabs alone are insufficient.
// PointerMotion is shared and lets focus-following recover when an application
// selected motion events before another window took focus. Every observed role
// shares this interest; only its property reads and management effects differ.
constexpr uint32_t kObservedWindowEventMask =
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
    WindowManager(Config config, std::string config_path);

    RunResult run();
    std::string const& restart_binary() const { return restart_binary_; }
    void prepare_restart();

private:
    // Atoms outside xcb-ewmh's set, interned once at startup.
    struct Atoms
    {
        xcb_atom_t wm_state;
        xcb_atom_t wm_delete_window;
        xcb_atom_t wm_take_focus;
        xcb_atom_t wm_s0;
        xcb_atom_t lwm_command;
        xcb_atom_t lwm_reply;
        xcb_atom_t lwm_window_class;
        xcb_atom_t lwm_state;
        xcb_atom_t lwm_restart;
        xcb_atom_t lwm_restart_owner;
    };

    // Last published window state outside plain properties; forget an externally
    // changed field to reconcile it. Domain decisions must use State, not caches.
    struct Output
    {
        bool mapped = false;
        bool hidden = false;
        std::optional<State::Presentation> presentation; ///< Last on-screen rectangle and border
        std::optional<uint32_t> border_color;
        std::optional<WindowStates> states; ///< Owned _NET_WM_STATE values, merged with other parties' atoms
        std::optional<bool> urgent;         ///< Urgency mirrored into the application's WM_HINTS
    };

    using StateUpdates = std::vector<std::pair<xcb_window_t, WindowStates>>;

    Connection conn_;
    Ewmh ewmh_;
    Atoms atoms_{ };
    State state_;
    std::unordered_map<xcb_window_t, Output> outputs_;
    // Last written bytes of every published property; nullopt records a deletion.
    // An unknown property is always written, so a fresh WM replaces stale values.
    std::map<std::pair<xcb_window_t, xcb_atom_t>, std::optional<std::string>> properties_;
    std::vector<xcb_window_t> fullscreen_owners_; ///< Logged ownership per monitor
    std::string config_path_;
    xcb_window_t wm_window_ = XCB_NONE; ///< Owns WM_S0, the EWMH supporting check, IPC and the restart marker
    std::optional<restart::Snapshot> handoff_; ///< Predecessor state, consumed during startup adoption

    // Obligations of the current operation that are not projections of state.
    std::set<xcb_window_t> configure_replies_;
    bool restack_requested_ = false;
    bool drain_requested_ = false;
    bool monitors_dirty_ = false;
    // Projection inputs outside State (outputs forgotten after an external
    // change). State changes are tracked by revision.
    bool presentation_dirty_ = false;
    uint64_t published_revision_ = UINT64_MAX;

    std::deque<xcb_generic_event_t> deferred_events_;
    bool pointer_grabbed_ = false; ///< Held exactly while State has a drag
    std::optional<RunResult> stop_; ///< Set when the event loop should end
    xcb_window_t restart_requester_ = XCB_NONE; ///< IPC caller of a restart, awaited before exec
    std::string restart_binary_;
    uint32_t last_event_time_ = XCB_CURRENT_TIME;
    // Latest timestamp from an actual input event (key/button/motion/crossing).
    // Unlike last_event_time_, never fed by PropertyNotify: user_time stamping
    // must reflect user interaction, not property churn.
    uint32_t last_input_time_ = XCB_CURRENT_TIME;
    xcb_cursor_t cursor_default_ = XCB_NONE;
    xcb_cursor_t cursor_resize_h_ = XCB_NONE;
    xcb_cursor_t cursor_resize_v_ = XCB_NONE;
    xcb_cursor_t current_root_cursor_ = XCB_NONE;

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
    void dispatch_event(xcb_generic_event_t const& event, size_t& remaining, std::chrono::steady_clock::time_point deadline);
    std::expected<void, std::string> reload_config(std::string_view source);
    bool launch_program(std::vector<std::string> const& command, std::string_view source);
    void close_window(xcb_window_t window);
    void send_protocol_message(xcb_window_t window, xcb_atom_t protocol, uint32_t timestamp, uint32_t d2 = 0);
    void set_root_cursor(xcb_cursor_t cursor);

    // wm_observe.cpp: X reads. Requests are pipelined; decoders are shared by
    // batch admission and single-property updates.
    // Only windows that exist and are not override-redirect (and are viewable when adopting).
    std::vector<WindowObservation> observe(std::span<xcb_window_t const> windows, bool adopting);
    WindowType window_type(xcb_get_property_cookie_t cookie) const;
    std::vector<xcb_atom_t> read_protocols(xcb_window_t window) const;
    xcb_get_property_cookie_t observe_user_time(xcb_window_t window, xcb_get_window_attributes_cookie_t cookie);
    std::optional<Geometry> read_window_geometry(xcb_window_t window) const;

    // wm_manage.cpp: admission adapters
    void scan_existing_windows(bool handoff);
    void manage(WindowObservation const& observed, bool adopting);

    // wm_transition.cpp: operation completion and publication
    void complete_transition();
    bool publish_clients(std::vector<State::Projected> const& clients);
    bool write_geometry(xcb_window_t window, Output& output, State::Presentation const& presentation);
    void send_configure_notify(xcb_window_t window, State::Presentation const& presentation);
    bool publish(
        xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint8_t format, std::optional<std::string_view> bytes
    );
    bool publish(xcb_window_t window, xcb_atom_t property, xcb_atom_t type, std::span<uint32_t const> words);
    bool publish_properties(Client const& client, Output& output, StateUpdates& updates);
    void publish_urgency(Client const& client, Output& output);
    void publish_fixtures();
    void publish_root(std::vector<State::Projected> const& clients, bool urgency_changed);
    void reconcile_stacking(State::FullscreenVisibility const& fullscreen, bool reassert);
    void withdraw_removed();
    void commit_focus(uint32_t time);
    void flush_and_drain_crossing();

    // wm_events.cpp: X event handlers
    void handle_event(xcb_generic_event_t const& event);
    void handle_map_request(xcb_map_request_event_t const& e);
    void handle_window_removal(xcb_window_t window);
    void handle_enter_notify(xcb_enter_notify_event_t const& e);
    void handle_motion_notify(xcb_motion_notify_event_t const& e);
    void handle_button_press(xcb_button_press_event_t const& e);
    void handle_button_release(xcb_button_release_event_t const& e);
    void handle_key_press(xcb_key_press_event_t const& e);
    void handle_client_message(xcb_client_message_event_t const& e);
    void handle_restack_message(xcb_client_message_event_t const& e);
    void handle_wm_state_change(xcb_client_message_event_t const& e);
    void handle_moveresize_window(xcb_client_message_event_t const& e);
    void handle_wm_moveresize(xcb_client_message_event_t const& e);
    void handle_configure_request(xcb_configure_request_event_t const& e);
    void handle_property_notify(xcb_property_notify_event_t const& event);
    bool grab_pointer(xcb_cursor_t cursor = XCB_NONE);
    void release_pointer();
    void begin_interaction(State::Interaction const& interaction, int16_t x, int16_t y, uint8_t button);
    xcb_cursor_t split_cursor(State::SplitHit const& split) const;

    // wm_actions.cpp: the one executor for key bindings and IPC
    std::expected<void, std::string> execute(Action const& action, std::string_view source);
    void warp_to_monitor(Monitor const& monitor);

    // wm_ipc.cpp
    void handle_command(xcb_window_t requester);
    std::string handle_request(std::string_view text);
    std::string state_json() const;

    // wm_restart.cpp
    void read_handoff();
};

} // namespace lwm
