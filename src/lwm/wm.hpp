#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/events.hpp"
#include "lwm/core/ewmh.hpp"
#include "lwm/core/floating.hpp"
#include "lwm/core/focus.hpp"
#include "lwm/core/invariants.hpp"
#include "lwm/core/ipc_server.hpp"
#include "lwm/core/policy.hpp"
#include "lwm/core/signals.hpp"
#include "lwm/core/state.hpp"
#include "lwm/core/types.hpp"
#include "lwm/core/window_rules.hpp"
#include "lwm/keybind/keybind.hpp"
#include "lwm/layout/layout.hpp"
#include <cassert>
#include <chrono>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <xcb/sync.h>

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

struct ClassificationResult
{
    WindowClassification classification;
    WindowRuleResult rule_result;
    xcb_window_t transient_for = XCB_NONE;
    WindowMatchInfo properties;
};

class WindowManager
{
public:
    WindowManager(Config config, SignalPipe& signals, std::string config_path);
    ~WindowManager();

    RunResult run();
    std::string const& restart_binary() const { return restart_binary_; }
    void prepare_restart();

private:
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

    Config config_;
    Connection conn_;
    Ewmh ewmh_;
    KeybindManager keybinds_;
    Layout layout_;

    State state_;
    State::Clients const& clients_ = state_.clients();
    std::vector<Monitor> const& monitors_ = state_.monitors();
    bool const& showing_desktop_ = state_.showing_desktop();
    std::unordered_map<xcb_window_t, std::chrono::steady_clock::time_point> pending_kills_;

    struct FocusCycle
    {
        size_t monitor, workspace;
        uint64_t next_recency, next_registration;
        xcb_window_t current;
        std::vector<xcb_window_t> order;
    };
    std::optional<FocusCycle> focus_cycle_;
    int32_t desktop_origin_x_ = 0;
    int32_t desktop_origin_y_ = 0;
    xcb_window_t const& active_window_ = state_.active_window();
    size_t const& focused_monitor_ = state_.focused_monitor();
    xcb_window_t wm_window_ = XCB_NONE;
    xcb_atom_t wm_s0_ = XCB_NONE;
    bool running_ = true;
    std::string config_path_;
    ipc::Server ipc_;
    xcb_atom_t wm_transient_for_ = XCB_NONE;
    xcb_atom_t wm_state_ = XCB_NONE;
    xcb_atom_t wm_change_state_ = XCB_NONE;
    xcb_atom_t utf8_string_ = XCB_NONE;
    xcb_atom_t lwm_ipc_socket_ = XCB_NONE;
    xcb_atom_t wm_protocols_ = XCB_NONE;
    xcb_atom_t wm_delete_window_ = XCB_NONE;
    xcb_atom_t wm_take_focus_ = XCB_NONE;
    xcb_atom_t wm_normal_hints_ = XCB_NONE;
    xcb_atom_t wm_hints_ = XCB_NONE;
    xcb_atom_t net_wm_ping_ = XCB_NONE;
    xcb_atom_t net_wm_sync_request_ = XCB_NONE;
    xcb_atom_t net_wm_sync_request_counter_ = XCB_NONE;
    xcb_atom_t net_close_window_ = XCB_NONE;
    xcb_atom_t net_wm_fullscreen_monitors_ = XCB_NONE;
    xcb_atom_t net_wm_user_time_ = XCB_NONE;
    xcb_atom_t net_wm_user_time_window_ = XCB_NONE;
    xcb_atom_t net_wm_state_focused_ = XCB_NONE;
    xcb_atom_t lwm_restart_client_ = XCB_NONE;
    xcb_atom_t lwm_restart_preferences_ = XCB_NONE;
    xcb_window_t restart_source_ = XCB_NONE;
    xcb_atom_t lwm_restart_owner_ = XCB_NONE;
    xcb_atom_t lwm_restart_state_ = XCB_NONE;
    xcb_atom_t lwm_restart_tiled_order_ = XCB_NONE;
    xcb_atom_t lwm_restart_floating_order_ = XCB_NONE;
    xcb_atom_t lwm_restart_ratios_ = XCB_NONE;
    xcb_atom_t lwm_window_class_ = XCB_NONE;
    bool restarting_ = false;
    bool is_restart_ = false;
    std::string restart_binary_;
    bool suppress_focus_ = false;
    uint32_t last_event_time_ = XCB_CURRENT_TIME;
    // Latest timestamp from an actual input event (key/button/motion/crossing).
    // Unlike last_event_time_, never fed by PropertyNotify — user_time stamping
    // must reflect user interaction, not property churn.
    uint32_t last_input_time_ = XCB_CURRENT_TIME;
    xcb_keysym_t last_toggle_keysym_ = XCB_NO_SYMBOL;
    xcb_timestamp_t last_toggle_release_time_ = 0;
    std::optional<Drag> drag_;

    // Cursor resources for tiled resize hover feedback
    xcb_cursor_t cursor_default_ = XCB_NONE;
    xcb_cursor_t cursor_resize_h_ = XCB_NONE;
    xcb_cursor_t cursor_resize_v_ = XCB_NONE;
    xcb_cursor_t current_root_cursor_ = XCB_NONE;

    // Double-click detection for gap ratio reset
    xcb_timestamp_t last_gap_click_time_ = 0;
    SplitAddress last_gap_click_address_{};
    size_t last_gap_click_monitor_ = 0;

    // Process-owned signal handlers and reload pipe survive WM reconstruction.
    SignalPipe& signals_;

    // One outer operation owns these effects; feature helpers never complete them.
    using TransitionEffects = State::TransitionEffects;
    void complete_transition();
    void
    dispatch_event(xcb_generic_event_t const& event, size_t& remaining, std::chrono::steady_clock::time_point deadline);
    void invalidate_monitor(size_t monitor, xcb_window_t preferred = XCB_NONE);
    void arrange_monitor(Monitor const& monitor);
    void realize_visibility(size_t monitor, xcb_window_t preferred);
    void commit_focus(TransitionEffects const& publication);
    void request_geometry(Client const& client);
    Geometry presentation_geometry(Client const& client) const;
    [[nodiscard]] bool write_geometry(Client const& client, Geometry geometry, uint32_t border);
    bool monitors_dirty_ = false;
    std::deque<xcb_generic_event_t> deferred_events_;

    // Scratchpad state
    using NamedScratchpadState = State::NamedScratchpadState;
    std::vector<NamedScratchpadState> const& named_scratchpads_ = state_.named_scratchpads();
    std::vector<xcb_window_t> const& scratchpad_pool_ = state_.scratchpad_pool();

    void create_wm_window();
    void setup_root();
    void grab_buttons();
    void claim_wm_ownership();
    void detect_monitors();
    Monitor create_fallback_monitor();
    void init_monitor_workspaces(Monitor& monitor);
    void scan_existing_windows();
    void run_autostart();
    void setup_ipc();
    void cleanup_ipc();
    void queue_event(EventType type, std::string json);

    void handle_event(xcb_generic_event_t const& event);
    void handle_map_request(xcb_map_request_event_t const& e);
    void map_desktop_window(xcb_window_t window);
    void map_dock_window(xcb_window_t window);
    void handle_window_removal(xcb_window_t window);
    void handle_enter_notify(xcb_enter_notify_event_t const& e);
    void handle_motion_notify(xcb_motion_notify_event_t const& e);
    void handle_button_press(xcb_button_press_event_t const& e);
    void handle_button_release(xcb_button_release_event_t const& e);
    void handle_key_press(xcb_key_press_event_t const& e);
    void handle_key_release(xcb_key_release_event_t const& e);
    bool is_auto_repeat_toggle(xcb_keysym_t keysym, xcb_timestamp_t time);
    void handle_client_message(xcb_client_message_event_t const& e);
    void handle_close_window_message(xcb_client_message_event_t const& e);
    void handle_fullscreen_monitors_message(xcb_client_message_event_t const& e);
    void handle_change_state_message(xcb_client_message_event_t const& e);
    void handle_current_desktop_message(xcb_client_message_event_t const& e);
    void handle_frame_extents_message(xcb_client_message_event_t const& e);
    void handle_restack_message(xcb_client_message_event_t const& e);
    void handle_wm_state_change(xcb_client_message_event_t const& e);
    void handle_active_window_request(xcb_client_message_event_t const& e);
    void handle_desktop_change(xcb_client_message_event_t const& e);
    void handle_moveresize_window(xcb_client_message_event_t const& e);
    void handle_wm_moveresize(xcb_client_message_event_t const& e);
    void handle_showing_desktop(xcb_client_message_event_t const& e);
    void handle_configure_request(xcb_configure_request_event_t const& e);
    void handle_property_notify(xcb_property_notify_event_t const& e);
    void handle_randr_screen_change();
    void handle_timeouts();
    std::string run_ipc_command(ipc::Command const& command);
    std::expected<void, std::string> reload_config();
    void emit_config_reload_result(std::expected<void, std::string> const& result, char const* source);
    std::expected<void, std::string> apply_config_reload(Config config);
    std::expected<void, std::string> validate_reload(Config const& config) const;
    void regrab_all_keys();
    void publish_appearance();
    void request_allowed_actions(Client const& client);
    void publish_allowed_actions(Client const& client);
    /// Publish the stable LWM classification used by external desktop tools.
    void publish_lwm_window_class(Client const& client);
    void reapply_rules_to_existing_windows();
    void apply_rule_result_to_window(xcb_window_t window, WindowRuleResult const& rule_result);
    void apply_rule_target_location(xcb_window_t window, WindowRuleResult const& rule_result);
    void apply_rule_floating_placement(xcb_window_t window, WindowRuleResult const& rule_result);
    void toggle_window_float(xcb_window_t window);
    std::optional<Geometry> read_window_geometry(xcb_window_t window) const;
    std::optional<Geometry> placement_parent_geometry(xcb_window_t window) const;

    void manage_client(
        xcb_window_t window,
        ClassificationResult const& initial,
        bool start_iconic = false,
        bool adopting = false
    );
    void read_initial_focus_hints(Client& client, bool honor_initial_state);
    void parse_initial_ewmh_state(Client& client);
    ClassificationResult classify_managed_window(xcb_window_t window, bool refresh_transient = false);
    /// _into populates user_time fields before insert; the window-keyed wrapper
    /// is for event-handler refresh after insert.
    void refresh_user_time_tracking_into(Client& client);
    void refresh_user_time_tracking(xcb_window_t window);
    void reevaluate_metadata(xcb_window_t window, WindowRuleResult const& previous);
    bool
    claim_pending_scratchpad(xcb_window_t window, WindowMatchInfo const& properties, WindowRuleResult const& rules);
    void relocate_to_transient_parent(xcb_window_t window, xcb_window_t previous_transient_for);

    void unmanage_window(xcb_window_t window);
    void focus_any_window(xcb_window_t window, bool record_user_time = true, uint32_t focus_timestamp = 0);
    /// Returns true when a target was focused; false when there is no focused
    /// monitor or no cycle candidates.
    bool cycle_focus(bool forward);
    void set_fullscreen(Client const& client, bool enabled);
    void set_fullscreen_monitors(Client const& client, FullscreenMonitors const& monitors);
    Geometry fullscreen_geometry_for_client(Client const& client) const;
    void set_iconic_state(xcb_window_t window, bool iconic);
    void iconify_window(xcb_window_t window);
    void deiconify_window(xcb_window_t window, bool focus);
    void kill_window(xcb_window_t window);
    void clear_focus();

    void invalidate_all_monitors();

    void switch_workspace(size_t ws);
    void toggle_workspace();
    void move_window_to_workspace(size_t ws);

    void focus_monitor(int direction); // -1 = left, +1 = right
    void move_window_to_monitor(int direction);

    bool launch_program(CommandConfig const& command);
    /// Returns true when the ratio actually changed (false: no focused
    /// monitor, or already clamped at the bound).
    bool adjust_master_ratio(double delta);
    void swap_focused_tiled(int offset);

    /// Lookup: nullable handle for X event boundaries where the window may not be managed.
    Client const* get_client(xcb_window_t window) const;
    /// Internal lookup after managed status is established; throws if the client is absent.
    Client const& require_client(xcb_window_t window) const;
    bool is_managed(xcb_window_t window) const { return clients_.contains(window); }
    bool window_is_iconic(xcb_window_t window) const
    {
        auto const* c = get_client(window);
        return c && c->iconic;
    }

    void request_urgency_update(Client const& client);
    void publish_urgency(Client const& client);

    Monitor const& focused_monitor() const { return monitors_[focused_monitor_]; }
    size_t monitor_index(Monitor const& m) const
    {
        auto idx = static_cast<size_t>(&m - monitors_.data());
        assert(idx < monitors_.size());
        return idx;
    }
    size_t wrap_monitor_index(int idx) const;
    void warp_to_monitor(Monitor const& monitor);
    void focus_or_fallback(Monitor const& monitor, bool record_user_time = true);
    void repair_focus_after_visibility_change(size_t preferred_monitor, bool record_user_time = false);
    focus::Context focus_context(size_t monitor) const;
    Monitor const* monitor_at_point(int16_t x, int16_t y);
    std::optional<uint32_t> get_raw_window_desktop(xcb_window_t window) const;
    std::optional<uint32_t> get_window_desktop(xcb_window_t window) const;
    bool is_sticky_desktop(xcb_window_t window) const;

    /// Outcome of resolving a window's _NET_WM_DESKTOP hint to (monitor, workspace).
    /// Distinguishes an absent/sticky hint from an invalid explicit assignment.
    enum class DesktopResolution
    {
        Resolved,   ///< monitor/workspace are valid indices.
        NoHint,     ///< Window has no _NET_WM_DESKTOP, or it is sticky (0xFFFFFFFF).
        OutOfRange, ///< Hint present but decodes to an invalid monitor or workspace index.
    };
    struct DesktopResolutionResult
    {
        DesktopResolution kind = DesktopResolution::NoHint;
        size_t monitor = 0;
        size_t workspace = 0;
    };
    struct FloatingPlacement
    {
        Geometry geometry;
        size_t monitor;
        size_t workspace;
        bool desktop_pinned;
    };
    FloatingPlacement initial_floating_placement(
        xcb_window_t window,
        ClassificationResult const& initial,
        DesktopResolutionResult const& desktop_target
    );
    DesktopResolutionResult resolve_window_desktop(xcb_window_t window) const;
    std::optional<xcb_window_t> transient_for_window(xcb_window_t window) const;
    bool should_be_visible(Client const& client) const;
    bool is_visible(Client const& client) const;
    bool is_suppressed_by_fullscreen(Client const& client) const;
    xcb_window_t effective_fullscreen_owner(size_t monitor) const;
    xcb_window_t select_fullscreen_owner_for_monitor(size_t monitor_idx, xcb_window_t preferred_owner = XCB_NONE) const;
    /// Realize the global order onto X and EWMH _NET_CLIENT_LIST_STACKING in
    /// one global pass.  X stacking is a single global order — a per-monitor
    /// pass cannot enforce cross-monitor invariants like floating-above-tile.
    void apply_stacking();
    bool is_override_redirect_window(xcb_window_t window) const;
    bool is_workspace_visible(size_t monitor_idx, size_t workspace_idx) const;
    void update_floating_monitor_for_geometry(Client const& client);
    void update_floating_monitor_for_geometry(Client const& client, Geometry const& geometry);
    uint32_t border_width_for_client(Client const& client) const;
    uint32_t border_color_for_client(Client const& client) const;
    void send_configure_notify(xcb_window_t window, Geometry const& geom, uint16_t border_width);
    void request_configure_notify(Client const& client);
    void publish_configure_notify(Client const& client);
    bool drag_active() const { return drag_.has_value(); }
    void begin_window_drag(
        xcb_window_t window,
        int16_t x,
        int16_t y,
        uint8_t button,
        floating::ResizeEdge edges = floating::ResizeEdge::None
    );
    void begin_tiled_resize(SplitHitResult const& hit, size_t monitor, int16_t x, int16_t y, uint8_t button);
    void update_drag(int16_t x, int16_t y);
    void end_drag(bool commit = true);
    void validate_drag(bool layout_changed);
    bool grab_pointer_for_drag(xcb_cursor_t cursor = XCB_NONE);
    void set_root_cursor(xcb_cursor_t cursor);
    void reset_split_ratio(SplitAddress address, size_t monitor_idx);
    std::vector<xcb_window_t> tiled_participants(Monitor const& monitor) const;

    struct SplitBorderHit
    {
        SplitHitResult hit;
        size_t monitor_idx;
    };
    std::optional<SplitBorderHit> try_hit_split_border(int16_t x, int16_t y);
    MousebindConfig const* resolve_mouse_binding(uint16_t state, uint8_t button) const;
    bool supports_protocol(xcb_window_t window, xcb_atom_t protocol) const;
    bool is_focus_eligible(Client const& client) const;
    bool is_focus_candidate(Client const& client) const;
    void send_wm_take_focus(Client const& client, uint32_t timestamp);
    void send_wm_ping(xcb_window_t window, uint32_t timestamp);
    void send_sync_request(Client const& client, uint32_t timestamp);
    void update_sync_state(Client const& client);
    void update_fullscreen_monitor_state(Client const& client);
    void update_focused_monitor_at_point(int16_t x, int16_t y);
    std::string get_window_name(xcb_window_t window);
    std::pair<std::string, std::string> get_wm_class(xcb_window_t window);
    uint32_t get_user_time(xcb_window_t window);
    void update_window_title(xcb_window_t window);
    void update_ewmh_workarea();

    void request_workarea_update();
    void refresh_workareas();

    using RelocationGeometry = State::RelocationGeometry;
    void flush_and_drain_crossing();

    void setup_ewmh();
    void update_ewmh_desktops();
    void request_client_list_update();
    void publish_client_list();
    void request_current_desktop_update();
    void publish_current_desktop();
    uint32_t get_ewmh_desktop_index(size_t monitor_idx, size_t workspace_idx) const;
    void switch_to_ewmh_desktop(uint32_t desktop);
    xcb_atom_t intern_atom(char const* name) const;

    std::string handle_notification_attention(xcb_window_t window);

    // Scratchpad operations
    std::optional<Geometry> detach_tiled_to_floating(Client const& client);
    void toggle_named_scratchpad(std::string_view name);
    void stash_to_scratchpad(xcb_window_t window);
    void cycle_scratchpad_pool();
    std::optional<std::string>
    match_scratchpad_for_window(WindowMatchInfo const& properties, WindowRuleResult const& rule_result);
    void hide_scratchpad_window(xcb_window_t window);
    void show_named_scratchpad_window(xcb_window_t window, ScratchpadConfig const& config);
    void show_pool_scratchpad_window(xcb_window_t window);
    void finalize_scratchpad_claim(xcb_window_t window, NamedScratchpadState const& state, std::string_view name);
    void init_scratchpad_state();
    ScratchpadConfig const* find_scratchpad_config(std::string_view name) const;
    NamedScratchpadState const* find_named_scratchpad(std::string_view name);
    xcb_window_t find_visible_pool_window() const;

    // Restart serialization atoms for scratchpad
    xcb_atom_t lwm_restart_scratchpad_name_ = XCB_NONE;
    xcb_atom_t lwm_restart_scratchpad_pool_ = XCB_NONE;

    // Exec restart
    void initiate_restart(std::string binary = {});
    void serialize_restart_state();
    bool restore_global_restart_state();
    void apply_restart_client_state(xcb_window_t window);
    void restore_window_ordering();
    void clean_restart_properties();
};

}
