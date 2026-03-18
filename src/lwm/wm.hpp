#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
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
#include <string>
#include <unordered_map>
#include <vector>
#include <xcb/sync.h>

namespace lwm {

class WindowManager
{
public:
    WindowManager(Config config, std::string config_path);
    ~WindowManager();

    void run();

private:
    struct DragState
    {
        bool active = false;
        bool resizing = false;
        bool tiled = false;
        xcb_window_t window = XCB_NONE;
        int16_t start_root_x = 0;
        int16_t start_root_y = 0;
        int16_t last_root_x = 0;
        int16_t last_root_y = 0;
        Geometry start_geometry;
    };

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
    std::vector<xcb_window_t> dock_windows_;
    std::vector<xcb_window_t> desktop_windows_;
    std::vector<xcb_window_t> floating_windows_;

    // Unified Client registry
    // This is the authoritative source of truth for all managed window state.
    // All state flags (fullscreen, iconic, sticky, above, below, maximized,
    // shaded, modal) are stored in Client records, not in separate sets.
    // See types.hpp Client struct for full documentation.
    std::unordered_map<xcb_window_t, Client> clients_;

    bool showing_desktop_ = false;
    std::unordered_map<xcb_window_t, std::chrono::steady_clock::time_point> pending_kills_;
    uint64_t next_client_order_ = 0;
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
    bool suppress_focus_ = false;
    uint32_t last_event_time_ = XCB_CURRENT_TIME;
    xcb_keysym_t last_toggle_keysym_ = XCB_NO_SYMBOL;
    xcb_timestamp_t last_toggle_release_time_ = 0;
    DragState drag_state_;
    std::vector<MouseBinding> mousebinds_;

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
    void handle_ipc();

    void handle_event(xcb_generic_event_t const& event);
    void handle_map_request(xcb_map_request_event_t const& e);
    void handle_window_removal(xcb_window_t window);
    void handle_enter_notify(xcb_enter_notify_event_t const& e);
    void handle_motion_notify(xcb_motion_notify_event_t const& e);
    void handle_button_press(xcb_button_press_event_t const& e);
    void handle_button_release(xcb_button_release_event_t const& e);
    void handle_key_press(xcb_key_press_event_t const& e);
    void handle_key_release(xcb_key_release_event_t const& e);
    void handle_client_message(xcb_client_message_event_t const& e);
    void handle_wm_state_change(xcb_client_message_event_t const& e);
    void handle_active_window_request(xcb_client_message_event_t const& e);
    void handle_desktop_change(xcb_client_message_event_t const& e);
    void handle_moveresize_window(xcb_client_message_event_t const& e);
    void handle_wm_moveresize(xcb_client_message_event_t const& e);
    void handle_showing_desktop(xcb_client_message_event_t const& e);
    void handle_configure_request(xcb_configure_request_event_t const& e);
    void handle_property_notify(xcb_property_notify_event_t const& e);
    void handle_expose(xcb_expose_event_t const& e);
    void handle_randr_screen_change();
    void handle_timeouts();
    std::string run_ipc_command(std::string const& command);
    std::expected<void, std::string> reload_config();
    std::expected<void, std::string> apply_config_reload(Config config);
    std::expected<void, std::string> validate_reload(Config const& config) const;
    void regrab_all_keys();
    void apply_appearance_reload();
    void update_allowed_actions(xcb_window_t window);
    void reapply_rules_to_existing_windows();
    void apply_rule_result_to_window(xcb_window_t window, WindowRuleResult const& rule_result);
    void convert_window_to_floating(xcb_window_t window);
    void convert_window_to_tiled(xcb_window_t window);
    Geometry current_window_geometry(xcb_window_t window) const;

    void manage_window(xcb_window_t window, bool start_iconic = false);
    void manage_floating_window(xcb_window_t window, bool start_iconic = false);
    void parse_initial_ewmh_state(Client& client);
    void init_user_time(xcb_window_t window);
    void apply_post_manage_states(xcb_window_t window, bool has_transient);
    WindowClassification classify_managed_window(xcb_window_t window);
    void refresh_user_time_tracking(xcb_window_t window);
    void reevaluate_managed_window(xcb_window_t window);
    void sync_managed_window_classification(xcb_window_t window, WindowClassification const& classification);
    void sync_window_visible_names(xcb_window_t window);
    void unmanage_window(xcb_window_t window);
    void unmanage_floating_window(xcb_window_t window);
    void focus_any_window(xcb_window_t window, bool record_user_time = true);
    void cycle_focus(bool forward);
    void set_fullscreen(xcb_window_t window, bool enabled);
    void clear_fullscreen_state(xcb_window_t window);
    void set_window_layer(xcb_window_t window, WindowLayer layer);
    void set_window_borderless(xcb_window_t window, bool enabled);
    void set_window_above(xcb_window_t window, bool enabled);
    void set_window_below(xcb_window_t window, bool enabled);
    void set_window_sticky(xcb_window_t window, bool enabled);
    void set_window_maximized(xcb_window_t window, bool horiz, bool vert);
    void apply_maximized_geometry(xcb_window_t window);
    void set_window_shaded(xcb_window_t window, bool enabled);
    void set_window_modal(xcb_window_t window, bool enabled);
    void apply_fullscreen_if_needed(
        xcb_window_t window,
        fullscreen_policy::ApplyContext context = fullscreen_policy::ApplyContext::StateTransition
    );
    void set_fullscreen_monitors(xcb_window_t window, FullscreenMonitors const& monitors);
    Geometry fullscreen_geometry_for_window(xcb_window_t window) const;
    void iconify_window(xcb_window_t window);
    void deiconify_window(xcb_window_t window, bool focus);
    void kill_window(xcb_window_t window);
    void clear_focus();

    void rearrange_monitor(Monitor& monitor);
    void rearrange_all_monitors();

    struct WorkspaceSwitchContext
    {
        size_t monitor_idx;
        size_t old_workspace;
        size_t new_workspace;
    };

    void perform_workspace_switch(WorkspaceSwitchContext const& ctx);
    void switch_workspace(int ws);
    void toggle_workspace();
    void move_window_to_workspace(int ws);

    void focus_monitor(int direction); // -1 = left, +1 = right
    void move_window_to_monitor(int direction);

    void launch_program(std::string const& path);

    Client* get_client(xcb_window_t window);
    Client const* get_client(xcb_window_t window) const;
    bool is_managed(xcb_window_t window) const { return clients_.contains(window); }

    bool is_client_fullscreen(xcb_window_t window) const;
    bool is_client_overlay(xcb_window_t window) const;
    bool is_client_iconic(xcb_window_t window) const;
    bool is_client_sticky(xcb_window_t window) const;
    bool is_client_above(xcb_window_t window) const;
    bool is_client_below(xcb_window_t window) const;
    bool is_client_maximized_horz(xcb_window_t window) const;
    bool is_client_maximized_vert(xcb_window_t window) const;
    bool is_client_shaded(xcb_window_t window) const;
    bool is_client_modal(xcb_window_t window) const;
    bool is_client_skip_taskbar(xcb_window_t window) const;
    bool is_client_skip_pager(xcb_window_t window) const;
    bool is_client_demands_attention(xcb_window_t window) const;

    void set_client_skip_taskbar(xcb_window_t window, bool enabled);
    void set_client_skip_pager(xcb_window_t window, bool enabled);
    void set_client_demands_attention(xcb_window_t window, bool enabled);

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
    Monitor* monitor_at_point(int16_t x, int16_t y);
    bool is_floating_window(xcb_window_t window) const;
    std::optional<size_t> monitor_index_for_window(xcb_window_t window) const;
    std::optional<size_t> workspace_index_for_window(xcb_window_t window) const;
    std::optional<uint32_t> get_raw_window_desktop(xcb_window_t window) const;
    std::optional<uint32_t> get_window_desktop(xcb_window_t window) const;
    bool is_sticky_desktop(xcb_window_t window) const;
    std::optional<std::pair<size_t, size_t>> resolve_window_desktop(xcb_window_t window) const;
    std::optional<xcb_window_t> transient_for_window(xcb_window_t window) const;
    bool is_window_in_visible_scope(xcb_window_t window) const;
    bool is_window_visible(xcb_window_t window) const;
    std::optional<xcb_window_t> fullscreen_owner_for_monitor(
        size_t monitor_idx,
        xcb_window_t preferred_owner = XCB_NONE
    ) const;
    bool is_suppressed_by_fullscreen(xcb_window_t window) const;
    void reconcile_fullscreen_for_monitor(size_t monitor_idx, xcb_window_t preferred_owner = XCB_NONE);
    void restack_transients(xcb_window_t parent);
    void restack_monitor_layers(size_t monitor_idx);
    bool is_override_redirect_window(xcb_window_t window) const;
    bool is_workspace_visible(size_t monitor_idx, size_t workspace_idx) const;
    void update_floating_monitor_for_geometry(xcb_window_t window);
    void apply_floating_geometry(xcb_window_t window);
    Geometry overlay_geometry_for_window(xcb_window_t window) const;
    uint32_t border_width_for_client(Client const& client) const;
    bool should_apply_focus_border(xcb_window_t window) const;
    void send_configure_notify(xcb_window_t window);
    void begin_drag(xcb_window_t window, bool resize, int16_t root_x, int16_t root_y);
    void begin_tiled_drag(xcb_window_t window, int16_t root_x, int16_t root_y);
    void update_drag(int16_t root_x, int16_t root_y);
    void end_drag();
    MouseBinding const* resolve_mouse_binding(uint16_t state, uint8_t button) const;
    bool supports_protocol(xcb_window_t window, xcb_atom_t protocol) const;
    bool is_focus_eligible(xcb_window_t window) const;
    bool should_set_input_focus(xcb_window_t window) const;
    void send_wm_take_focus(xcb_window_t window, uint32_t timestamp);
    void send_wm_ping(xcb_window_t window, uint32_t timestamp);
    void send_sync_request(xcb_window_t window, uint32_t timestamp);
    bool wait_for_sync_counter(xcb_window_t window, uint64_t expected_value);
    void update_sync_state(xcb_window_t window);
    void update_fullscreen_monitor_state(xcb_window_t window);
    void update_focused_monitor_at_point(int16_t x, int16_t y);
    std::string get_window_name(xcb_window_t window);
    std::pair<std::string, std::string> get_wm_class(xcb_window_t window);
    uint32_t get_user_time(xcb_window_t window);
    void update_window_title(xcb_window_t window);
    void update_ewmh_workarea();

    void update_struts();
    void unmanage_dock_window(xcb_window_t window);
    void unmanage_desktop_window(xcb_window_t window);

    // Tiled window membership helpers (atomically update workspace list + client fields + EWMH)
    void add_tiled_to_workspace(xcb_window_t window, size_t monitor_idx, size_t workspace_idx);
    void remove_tiled_from_workspace(xcb_window_t window, size_t monitor_idx, size_t workspace_idx);

    // Off-screen visibility management (DWM-style)
    void hide_window(xcb_window_t window);
    void show_window(xcb_window_t window);

    // Derived visibility: sync physical visibility to match policy state
    void sync_visibility_for_monitor(size_t monitor_idx);
    void sync_visibility_all();

    void setup_ewmh();
    void update_ewmh_desktops();
    void update_ewmh_client_list();
    void update_ewmh_current_desktop();
    uint32_t get_ewmh_desktop_index(size_t monitor_idx, size_t workspace_idx) const;
    void switch_to_ewmh_desktop(uint32_t desktop);
    void clear_all_borders();
    xcb_atom_t intern_atom(char const* name) const;
};

}
