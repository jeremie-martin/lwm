#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/events.hpp"
#include "lwm/core/ewmh.hpp"
#include "lwm/core/invariants.hpp"
#include "lwm/core/policy.hpp"
#include "lwm/core/types.hpp"
#include "lwm/core/window_rules.hpp"
#include "lwm/keybind/keybind.hpp"
#include "lwm/layout/layout.hpp"
#include <cassert>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <xcb/sync.h>

namespace lwm {

/// Event mask selected on every managed (tiled/floating) client window.
/// Does NOT include ButtonPress — that is an exclusive mask in X11 (only one
/// client per window).  Applications that select it would cause our entire
/// ChangeWindowAttributes to fail with BadAccess, silently dropping EnterWindow.
/// Click-to-focus uses passive grabs on the managed window itself; modifier
/// button bindings still install root passive grabs for root/gap clicks, but
/// managed-window presses are handled in handle_button_press() because
/// ReplayPointer skips ancestor passive grabs.
///
/// PointerMotion is NOT exclusive — multiple clients can select it on the same
/// window.  We need it so that the WM receives MotionNotify even when the app
/// also selects PointerMotion (e.g. terminals).  Without it, focus-follows-mouse
/// fails to recover after a new window steals focus on a different monitor.
constexpr uint32_t kManagedWindowEventMask =
    XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_POINTER_MOTION;

enum class RunResult
{
    Exit,
    Restart
};

struct ClassificationResult
{
    WindowClassification classification;
    WindowRuleResult rule_result;
};

class WindowManager
{
public:
    WindowManager(Config config, std::string config_path);
    ~WindowManager();

    RunResult run();
    std::string const& restart_binary() const { return restart_binary_; }
    void prepare_restart();

private:
    struct NoDrag {};

    struct FloatingMove
    {
        xcb_window_t window = XCB_NONE;
        int16_t start_root_x = 0;
        int16_t start_root_y = 0;
        int16_t last_root_x = 0;
        int16_t last_root_y = 0;
        Geometry start_geometry;
    };

    struct FloatingResize
    {
        xcb_window_t window = XCB_NONE;
        int16_t start_root_x = 0;
        int16_t start_root_y = 0;
        int16_t last_root_x = 0;
        int16_t last_root_y = 0;
        Geometry start_geometry;
    };

    struct TiledMove
    {
        xcb_window_t window = XCB_NONE;
        int16_t start_root_x = 0;
        int16_t start_root_y = 0;
        int16_t last_root_x = 0;
        int16_t last_root_y = 0;
        Geometry start_geometry;
    };

    struct TiledResize
    {
        size_t monitor_idx = 0;
        size_t workspace_idx = 0; // abort drag if workspace changes
        SplitAddress address {};
        SplitDirection direction = SplitDirection::Horizontal;
        double start_ratio = 0.5;
        int32_t available_extent = 0; // total available pixels along split axis
        int16_t start_root_x = 0;
        int16_t start_root_y = 0;
        int16_t last_root_x = 0;
        int16_t last_root_y = 0;
    };

    using DragState = std::variant<NoDrag, FloatingMove, FloatingResize, TiledMove, TiledResize>;

    struct MouseBinding
    {
        uint16_t modifier = 0;
        uint8_t button = 0;
        std::string action;
    };

    Config config_;
    Connection conn_;
    Ewmh ewmh_;
    KeybindManager keybinds_;
    Layout layout_;
    WindowRules window_rules_;

    std::vector<Monitor> monitors_;
    // Unified Client registry
    // This is the authoritative source of truth for all managed window state.
    // All state flags (fullscreen, iconic, sticky, above, below, maximized,
    // modal) are stored in Client records, not in separate sets.
    // See types.hpp Client struct for full documentation.
    std::unordered_map<xcb_window_t, Client> clients_;

    bool showing_desktop_ = false;
    std::unordered_map<xcb_window_t, std::chrono::steady_clock::time_point> pending_kills_;
    uint64_t next_client_order_ = 0;
    uint64_t next_mru_order_ = 0;
    int32_t desktop_origin_x_ = 0;
    int32_t desktop_origin_y_ = 0;
    xcb_window_t active_window_ = XCB_NONE;
    size_t focused_monitor_ = 0;
    xcb_window_t wm_window_ = XCB_NONE;
    xcb_atom_t wm_s0_ = XCB_NONE;
    bool running_ = true;
    std::string config_path_;
    std::string ipc_socket_path_;
    int ipc_listener_fd_ = -1;
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
    xcb_atom_t lwm_restart_state_ = XCB_NONE;
    xcb_atom_t lwm_restart_tiled_order_ = XCB_NONE;
    xcb_atom_t lwm_restart_floating_order_ = XCB_NONE;
    xcb_atom_t lwm_restart_ratios_ = XCB_NONE;
    bool restarting_ = false;
    bool is_restart_ = false;
    std::string restart_binary_;
    bool suppress_focus_ = false;
    uint32_t last_event_time_ = XCB_CURRENT_TIME;
    xcb_keysym_t last_toggle_keysym_ = XCB_NO_SYMBOL;
    xcb_timestamp_t last_toggle_release_time_ = 0;
    DragState drag_state_;
    std::vector<MouseBinding> mousebinds_;

    // Cursor resources for tiled resize hover feedback
    xcb_cursor_t cursor_default_ = XCB_NONE;
    xcb_cursor_t cursor_resize_h_ = XCB_NONE;
    xcb_cursor_t cursor_resize_v_ = XCB_NONE;
    xcb_cursor_t current_root_cursor_ = XCB_NONE;

    // Double-click detection for gap ratio reset
    xcb_timestamp_t last_gap_click_time_ = 0;
    SplitAddress last_gap_click_address_ {};
    size_t last_gap_click_monitor_ = 0;

    struct IpcClient
    {
        int fd = -1;
        std::string buffer;
        std::chrono::steady_clock::time_point deadline;
    };
    std::optional<IpcClient> pending_ipc_;

    // SIGHUP self-pipe: handler writes to [1], main loop polls [0]
    int signal_pipe_[2] = {-1, -1};

    // Event subscribers (long-lived IPC connections)
    struct Subscriber
    {
        int fd = -1;
        uint32_t event_mask = Event_All;
    };
    std::vector<Subscriber> subscribers_;
    static constexpr size_t MAX_SUBSCRIBERS = 8;

    bool stacking_dirty_ = false;
    std::vector<xcb_window_t> cached_stacking_order_;

    // Scratchpad state
    struct NamedScratchpadState
    {
        struct Empty {};
        struct LaunchPending {};
        struct Claimed
        {
            xcb_window_t window = XCB_NONE;
        };

        std::string name;
        std::variant<Empty, LaunchPending, Claimed> state = Empty {};

        xcb_window_t window() const
        {
            if (auto const* claimed = std::get_if<Claimed>(&state))
                return claimed->window;
            return XCB_NONE;
        }

        bool pending_launch() const
        {
            return std::holds_alternative<LaunchPending>(state);
        }

        void mark_empty()
        {
            state = Empty {};
        }

        void mark_launch_pending()
        {
            state = LaunchPending {};
        }

        void mark_claimed(xcb_window_t claimed_window)
        {
            state = Claimed { claimed_window };
        }
    };
    std::vector<NamedScratchpadState> named_scratchpads_;
    std::vector<xcb_window_t> scratchpad_pool_; ///< Generic pool, MRU-ordered (back = most recent)

    struct CompiledScratchpadMatcher
    {
        std::string name;
        std::optional<std::regex> class_regex;
        std::optional<std::regex> instance_regex;
        std::optional<std::regex> title_regex;
    };
    std::vector<CompiledScratchpadMatcher> scratchpad_matchers_;

    void create_wm_window();
    void setup_root();
    void grab_buttons();
    void claim_wm_ownership();
    void init_mousebinds();
    void detect_monitors();
    void create_fallback_monitor();
    void init_monitor_workspaces(Monitor& monitor);
    void scan_existing_windows();
    void run_autostart();
    void setup_ipc();
    void cleanup_ipc();
    void accept_ipc_client();
    void process_ipc_client();
    void close_ipc_client();
    void emit_event(EventType type, std::string_view json);
    void cleanup_dead_subscribers();

    void handle_event(xcb_generic_event_t const& event);
    void handle_map_request(xcb_map_request_event_t const& e);
    void map_desktop_window(xcb_window_t window);
    void map_dock_window(xcb_window_t window);
    void map_floating_window(
        xcb_window_t window,
        WindowClassification const& classification,
        WindowRuleResult const& rule_result,
        bool start_iconic,
        bool urgent);
    void map_tiled_window(
        xcb_window_t window,
        WindowClassification const& classification,
        WindowRuleResult const& rule_result,
        bool start_iconic,
        bool urgent);
    void apply_initial_state_flags(Client& client, WindowRuleResult const& rule);
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
    std::optional<std::string> run_ipc_command(std::string const& command);
    std::expected<void, std::string> reload_config();
    void emit_config_reload_result(std::expected<void, std::string> const& result, char const* source);
    std::expected<void, std::string> apply_config_reload(Config config);
    std::expected<void, std::string> validate_reload(Config const& config) const;
    void regrab_all_keys();
    void apply_appearance_reload();
    void update_allowed_actions(Client const& client);
    void reapply_rules_to_existing_windows();
    void apply_rule_result_to_window(xcb_window_t window, WindowRuleResult const& rule_result);
    void apply_rule_target_location(xcb_window_t window, WindowRuleResult const& rule_result);
    void apply_rule_floating_placement(xcb_window_t window, WindowRuleResult const& rule_result);
    void convert_window_to_floating(xcb_window_t window);
    void convert_window_to_tiled(xcb_window_t window, std::optional<Geometry> prior_floating = std::nullopt);
    void toggle_window_float(xcb_window_t window);
    Geometry current_window_geometry(xcb_window_t window) const;

    void manage_window(xcb_window_t window, bool start_iconic = false);
    void manage_floating_window(xcb_window_t window, bool start_iconic = false);
    /// Populate accepts_input / supports_take_focus on a client struct from X11 properties.
    /// The _into form takes a reference so it can be called before the client is inserted
    /// into clients_; the window-keyed wrapper is for event-handler use after insert.
    void cache_focus_hints_into(Client& client);
    void cache_focus_hints(xcb_window_t window);
    void parse_initial_ewmh_state(Client& client);
    void apply_post_manage_states(xcb_window_t window, bool has_transient);
    ClassificationResult classify_managed_window(xcb_window_t window);
    /// Same split: _into populates user_time fields before insert; window-keyed wrapper
    /// is for event-handler refresh after insert.
    void refresh_user_time_tracking_into(Client& client);
    void refresh_user_time_tracking(xcb_window_t window);
    void reevaluate_managed_window(xcb_window_t window);
    void sync_managed_window_classification(xcb_window_t window, ClassificationResult const& result);
    bool sync_kind_and_layer(xcb_window_t window, WindowClassification::Kind desired_kind, WindowRuleResult const& rule_result);
    void relocate_to_transient_parent(xcb_window_t window, xcb_window_t previous_transient_for);
    void apply_classification_state(
        xcb_window_t window,
        WindowClassification const& classification,
        WindowRuleResult const& rule_result,
        bool has_transient);

    void unmanage_window(xcb_window_t window);
    void unmanage_floating_window(xcb_window_t window);
    void focus_any_window(xcb_window_t window, bool record_user_time = true);
    void cycle_focus(bool forward);
    void set_fullscreen(Client& client, bool enabled);
    void clear_fullscreen_state(Client& client);
    void set_window_layer(Client& client, WindowLayer layer);
    void set_window_borderless(Client& client, bool enabled);
    void set_window_layer_hint(Client& client, LayerHint hint);
    void set_window_sticky(Client& client, bool enabled);
    void set_window_maximized(Client& client, bool horiz, bool vert);
    void apply_maximized_geometry(Client& client);
    void set_window_modal(Client& client, bool enabled);
    void apply_fullscreen_if_needed(Client& client);
    void set_fullscreen_monitors(Client& client, FullscreenMonitors const& monitors);
    Geometry fullscreen_geometry_for_client(Client const& client) const;
    void set_iconic_state(xcb_window_t window, bool iconic);
    void iconify_window(xcb_window_t window);
    void deiconify_window(xcb_window_t window, bool focus);
    void kill_window(xcb_window_t window);
    void clear_focus();

    void rearrange_monitor(Monitor& monitor, bool geometry_only = false);
    void rearrange_all_monitors();

    /// Validated input for perform_workspace_switch. Construct via validate() — the
    /// private constructor prevents callers from passing a no-op or out-of-range
    /// switch, so perform_workspace_switch can act without re-checking.
    class WorkspaceSwitchContext
    {
    public:
        /// Returns nullopt if target_workspace equals current or is out of range.
        static std::optional<WorkspaceSwitchContext>
        validate(size_t monitor_idx, Monitor const& monitor, size_t target_workspace);

        size_t monitor_idx() const { return monitor_idx_; }
        size_t old_workspace() const { return old_workspace_; }
        size_t new_workspace() const { return new_workspace_; }

    private:
        WorkspaceSwitchContext(size_t monitor_idx, size_t old_workspace, size_t new_workspace)
            : monitor_idx_(monitor_idx), old_workspace_(old_workspace), new_workspace_(new_workspace) {}
        size_t monitor_idx_;
        size_t old_workspace_;
        size_t new_workspace_;
    };

    void perform_workspace_switch(WorkspaceSwitchContext const& ctx);
    void switch_workspace(size_t ws);
    void toggle_workspace();
    void move_window_to_workspace(size_t ws);

    void focus_monitor(int direction); // -1 = left, +1 = right
    void move_window_to_monitor(int direction);

    void launch_program(CommandConfig const& command);
    void adjust_master_ratio(double delta);

    /// Lookup: nullable handle for X event boundaries where the window may not be managed.
    Client* get_client(xcb_window_t window);
    Client const* get_client(xcb_window_t window) const;
    /// Require: asserts the client exists. Use from internal funnels that already proved
    /// managed status (iterating clients_, post-manage finalization, operations on
    /// active_window_, hotplug-plan apply, restart-state apply).
    Client& require_client(xcb_window_t window);
    Client const& require_client(xcb_window_t window) const;
    bool is_managed(xcb_window_t window) const { return clients_.contains(window); }
    bool window_is_iconic(xcb_window_t window) const
    {
        auto const* c = get_client(window);
        return c && c->iconic;
    }

    void set_client_skip_taskbar(Client& client, bool enabled);
    void set_client_skip_pager(Client& client, bool enabled);
    void set_client_urgency(Client& client, UrgencySource source, bool enabled);
    void clear_client_urgency(Client& client);
    void sync_client_urgency_state(Client& client);

    Monitor& focused_monitor() { return monitors_[focused_monitor_]; }
    Monitor const& focused_monitor() const { return monitors_[focused_monitor_]; }
    size_t monitor_index(Monitor const& m) const
    {
        auto idx = static_cast<size_t>(&m - monitors_.data());
        assert(idx < monitors_.size());
        return idx;
    }
    size_t wrap_monitor_index(int idx) const;
    void warp_to_monitor(Monitor const& monitor);
    void focus_or_fallback(Monitor& monitor, bool record_user_time = true);
    std::vector<focus_policy::FloatingCandidate> build_floating_candidates() const;
    Monitor* monitor_at_point(int16_t x, int16_t y);
    bool is_floating_window(xcb_window_t window) const;
    std::optional<uint32_t> get_raw_window_desktop(xcb_window_t window) const;
    std::optional<uint32_t> get_window_desktop(xcb_window_t window) const;
    bool is_sticky_desktop(xcb_window_t window) const;

    /// Outcome of resolving a window's _NET_WM_DESKTOP hint to (monitor, workspace).
    /// Discriminates the three reasons a hint can fail to land at a concrete location,
    /// so callers can distinguish "the window has no opinion" from "the app sent garbage".
    enum class DesktopResolution
    {
        Resolved,    ///< monitor/workspace are valid indices.
        NoHint,      ///< Window has no _NET_WM_DESKTOP, or it is sticky (0xFFFFFFFF).
        OutOfRange,  ///< Hint present but decodes to an invalid monitor or workspace index.
    };
    struct DesktopResolutionResult
    {
        DesktopResolution kind = DesktopResolution::NoHint;
        size_t monitor = 0;
        size_t workspace = 0;
    };
    DesktopResolutionResult resolve_window_desktop(xcb_window_t window) const;
    std::optional<xcb_window_t> transient_for_window(xcb_window_t window) const;
    bool should_be_visible(Client const& client) const;
    bool is_physically_visible(Client const& client) const;
    bool is_suppressed_by_fullscreen(Client const& client) const;
    int compute_stack_layer(Client const& client) const;
    void release_fullscreen_owner(Client const& client);
    void claim_fullscreen_owner(Client const& client);
    void update_fullscreen_owner_after_visibility_change(size_t monitor_idx);
    void finalize_after_desktop_move(
        xcb_window_t window, bool was_active, size_t target_monitor, size_t target_workspace);
    void restack_transients(xcb_window_t parent);
    void restack_monitor_layers(size_t monitor_idx);
    bool is_override_redirect_window(xcb_window_t window) const;
    bool is_workspace_visible(size_t monitor_idx, size_t workspace_idx) const;
    void update_floating_monitor_for_geometry(Client& client);
    void apply_floating_geometry(Client& client);
    Geometry overlay_geometry_for_client(Client const& client) const;
    uint32_t border_width_for_client(Client const& client) const;
    uint32_t border_color_for_client(Client const& client) const;
    bool should_apply_focus_border(Client const& client) const;
    void send_configure_notify(xcb_window_t window, Geometry const& geom, uint16_t border_width);
    void send_configure_notify(Client const& client);
    bool drag_active() const;
    void begin_floating_move(xcb_window_t window, int16_t root_x, int16_t root_y);
    void begin_floating_resize(xcb_window_t window, int16_t root_x, int16_t root_y);
    void begin_tiled_drag(xcb_window_t window, int16_t root_x, int16_t root_y);
    void begin_tiled_resize(SplitHitResult const& hit, size_t monitor_idx, int16_t root_x, int16_t root_y);
    void record_drag_position(int16_t root_x, int16_t root_y);
    void update_drag(int16_t root_x, int16_t root_y);
    void end_drag();
    void grab_pointer_for_drag(xcb_cursor_t cursor = XCB_NONE);
    void set_root_cursor(xcb_cursor_t cursor);
    void reset_split_ratio(SplitAddress address, size_t monitor_idx);
    size_t visible_tiled_count(Monitor const& monitor) const;

    struct SplitBorderHit
    {
        SplitHitResult hit;
        size_t monitor_idx;
    };
    std::optional<SplitBorderHit> try_hit_split_border(int16_t x, int16_t y);
    MouseBinding const* resolve_mouse_binding(uint16_t state, uint8_t button) const;
    bool supports_protocol(xcb_window_t window, xcb_atom_t protocol) const;
    bool is_focus_eligible(Client const& client) const;
    bool is_focus_candidate(xcb_window_t window) const
    {
        auto const* c = get_client(window);
        return c && !c->iconic && is_focus_eligible(*c);
    }
    void send_wm_take_focus(Client const& client, uint32_t timestamp);
    void send_wm_ping(xcb_window_t window, uint32_t timestamp);
    void send_sync_request(Client& client, uint32_t timestamp);
    void update_sync_state(Client& client);
    void update_fullscreen_monitor_state(Client& client);
    void update_focused_monitor_at_point(int16_t x, int16_t y);
    std::string get_window_name(xcb_window_t window);
    std::pair<std::string, std::string> get_wm_class(xcb_window_t window);
    uint32_t get_user_time(xcb_window_t window);
    void update_window_title(xcb_window_t window);
    void update_ewmh_workarea();

    void update_struts();
    void unmanage_dock_window(xcb_window_t window);
    void unmanage_desktop_window(xcb_window_t window);

    // Window workspace helper (atomically update client fields + EWMH desktop)
    void assign_window_workspace(Client& client, size_t monitor_idx, size_t workspace_idx);

    bool move_tiled_client_to_workspace(
        Client& client,
        size_t target_monitor,
        size_t target_workspace,
        std::optional<size_t> insert_index = std::nullopt);
    bool move_floating_client_to_workspace(
        Client& client,
        size_t target_monitor,
        size_t target_workspace,
        bool place_on_monitor_change);

    // Tiled window membership helpers (atomically update workspace list + client fields + EWMH)
    void add_tiled_to_workspace(Client& client, size_t monitor_idx, size_t workspace_idx);
    void remove_tiled_from_workspace(Client const& client, size_t monitor_idx, size_t workspace_idx);

    // Off-screen visibility management (DWM-style)
    void hide_window(Client& client);
    void show_window(Client& client);
    void flush_and_drain_crossing();

    // Derived visibility: sync physical visibility to match policy state
    void sync_visibility_for_monitor(size_t monitor_idx);
    void sync_visibility_all();

    // Funnel: refresh fullscreen ownership, sync visibility, then re-tile.
    // Use at any unconditional site that previously called these three in order.
    void finalize_visibility_on_monitor(size_t monitor_idx);

    void setup_ewmh();
    void update_ewmh_desktops();
    void update_ewmh_client_list();
    void flush_stacking_list();
    void update_ewmh_current_desktop();
    uint32_t get_ewmh_desktop_index(size_t monitor_idx, size_t workspace_idx) const;
    void switch_to_ewmh_desktop(uint32_t desktop);
    void clear_all_borders();
    xcb_atom_t intern_atom(char const* name) const;

    std::string handle_notification_attention(xcb_window_t window);

    // Scratchpad operations
    std::optional<Geometry> detach_tiled_to_floating(Client& client);
    void toggle_named_scratchpad(std::string_view name);
    void stash_to_scratchpad(xcb_window_t window);
    void cycle_scratchpad_pool();
    std::optional<std::string> match_scratchpad_for_window(xcb_window_t window, WindowRuleResult const& rule_result);
    void hide_scratchpad_window(xcb_window_t window);
    void show_named_scratchpad_window(xcb_window_t window, ScratchpadConfig const& config);
    void show_pool_scratchpad_window(xcb_window_t window);
    void release_scratchpad_window(xcb_window_t window);
    void finalize_scratchpad_claim(xcb_window_t window, NamedScratchpadState& state, std::string_view name);
    void init_scratchpad_state();
    void rebuild_scratchpad_matchers();
    ScratchpadConfig const* find_scratchpad_config(std::string_view name) const;
    NamedScratchpadState* find_named_scratchpad(std::string_view name);
    xcb_window_t find_visible_pool_window() const;

    // Restart serialization atoms for scratchpad
    xcb_atom_t lwm_restart_scratchpad_name_ = XCB_NONE;
    xcb_atom_t lwm_restart_scratchpad_pool_ = XCB_NONE;

    // Hot-reload (exec-based restart)
    void initiate_restart(std::string binary = {});
    void serialize_restart_state();
    bool restore_global_restart_state();
    void apply_restart_client_state(xcb_window_t window);
    void restore_window_ordering();
    void clean_restart_properties();
};

}
