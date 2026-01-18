#pragma once

#include "lwm/bar/bar.hpp"
#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/ewmh.hpp"
#include "lwm/core/types.hpp"
#include "lwm/keybind/keybind.hpp"
#include "lwm/layout/layout.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xcb/sync.h>

namespace lwm {

class WindowManager
{
public:
    explicit WindowManager(Config config);

    void run();

private:
    struct FloatingWindow
    {
        xcb_window_t id = XCB_NONE;
        size_t monitor = 0;
        size_t workspace = 0;
        Geometry geometry;
        std::string name;
        xcb_window_t transient_for = XCB_NONE;
    };

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

    // Note: FullscreenMonitors moved to types.hpp as part of unified Client refactor

    struct ActiveWindowInfo
    {
        size_t monitor = 0;
        size_t workspace = 0;
        std::string title;
    };

    Config config_;
    Connection conn_;
    Ewmh ewmh_;
    KeybindManager keybinds_;
    Layout layout_;
    std::optional<StatusBar> bar_;

    std::vector<Monitor> monitors_;
    std::vector<xcb_window_t> dock_windows_;
    std::vector<xcb_window_t> desktop_windows_;
    std::vector<FloatingWindow> floating_windows_;

    // ─────────────────────────────────────────────────────────────────────────
    // Unified client registry (Phase 2 of Client refactor)
    //
    // This map is the authoritative source of truth for all managed windows.
    // During the migration period, it coexists with the legacy data structures
    // above. Once migration is complete, the scattered sets/maps below will be
    // removed and all state will be accessed through clients_.
    // ─────────────────────────────────────────────────────────────────────────
    std::unordered_map<xcb_window_t, Client> clients_;

    std::unordered_map<xcb_window_t, uint32_t> wm_unmapped_windows_;
    std::unordered_set<xcb_window_t> fullscreen_windows_;
    std::unordered_set<xcb_window_t> above_windows_;
    std::unordered_set<xcb_window_t> below_windows_;
    std::unordered_set<xcb_window_t> iconic_windows_;
    std::unordered_set<xcb_window_t> sticky_windows_;
    std::unordered_set<xcb_window_t> maximized_horz_windows_;
    std::unordered_set<xcb_window_t> maximized_vert_windows_;
    std::unordered_set<xcb_window_t> shaded_windows_;
    std::unordered_set<xcb_window_t> modal_windows_;
    std::unordered_set<xcb_window_t> skip_taskbar_windows_;
    std::unordered_set<xcb_window_t> skip_pager_windows_;
    bool showing_desktop_ = false;
    std::unordered_map<xcb_window_t, Geometry> fullscreen_restore_;
    std::unordered_map<xcb_window_t, Geometry> maximize_restore_;
    std::unordered_map<xcb_window_t, FullscreenMonitors> fullscreen_monitors_;
    std::unordered_map<xcb_window_t, xcb_sync_counter_t> sync_counters_;
    std::unordered_map<xcb_window_t, uint64_t> sync_values_;
    std::unordered_map<xcb_window_t, std::chrono::steady_clock::time_point> pending_kills_;
    std::unordered_map<xcb_window_t, std::chrono::steady_clock::time_point> pending_pings_;
    std::unordered_map<xcb_window_t, uint32_t> user_times_;
    std::unordered_map<xcb_window_t, xcb_window_t> user_time_windows_; // client → user time window
    std::unordered_map<xcb_window_t, uint64_t> client_order_;
    uint64_t next_client_order_ = 0;
    int32_t desktop_origin_x_ = 0;
    int32_t desktop_origin_y_ = 0;
    xcb_window_t active_window_ = XCB_NONE;
    size_t focused_monitor_ = 0; // Active monitor index (window focus tracked separately).
    xcb_window_t wm_window_ = XCB_NONE;
    xcb_atom_t wm_s0_ = XCB_NONE;
    bool running_ = true;
    xcb_atom_t wm_transient_for_ = XCB_NONE;
    xcb_atom_t wm_state_ = XCB_NONE;
    xcb_atom_t wm_change_state_ = XCB_NONE;
    xcb_atom_t utf8_string_ = XCB_NONE;
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
    DragState drag_state_;
    std::vector<MouseBinding> mousebinds_;

    void create_wm_window();
    void setup_root();
    void grab_buttons();
    void claim_wm_ownership();
    void init_mousebinds();
    void detect_monitors();
    void create_fallback_monitor();
    void setup_monitor_bars();
    void init_monitor_workspaces(Monitor& monitor);
    void scan_existing_windows();

    void handle_event(xcb_generic_event_t const& event);
    void handle_map_request(xcb_map_request_event_t const& e);
    void handle_window_removal(xcb_window_t window);
    void handle_enter_notify(xcb_enter_notify_event_t const& e);
    void handle_motion_notify(xcb_motion_notify_event_t const& e);
    void handle_button_press(xcb_button_press_event_t const& e);
    void handle_button_release(xcb_button_release_event_t const& e);
    void handle_key_press(xcb_key_press_event_t const& e);
    void handle_client_message(xcb_client_message_event_t const& e);
    void handle_configure_request(xcb_configure_request_event_t const& e);
    void handle_property_notify(xcb_property_notify_event_t const& e);
    void handle_expose(xcb_expose_event_t const& e);
    void handle_randr_screen_change();
    void handle_timeouts();

    void manage_window(xcb_window_t window, bool start_iconic = false);
    void manage_floating_window(xcb_window_t window, bool start_iconic = false);
    void unmanage_window(xcb_window_t window);
    void unmanage_floating_window(xcb_window_t window);
    void focus_window(xcb_window_t window);
    void focus_floating_window(xcb_window_t window);
    void set_fullscreen(xcb_window_t window, bool enabled);
    void set_window_above(xcb_window_t window, bool enabled);
    void set_window_below(xcb_window_t window, bool enabled);
    void set_window_sticky(xcb_window_t window, bool enabled);
    void set_window_maximized(xcb_window_t window, bool horiz, bool vert);
    void apply_maximized_geometry(xcb_window_t window);
    void set_window_shaded(xcb_window_t window, bool enabled);
    void set_window_modal(xcb_window_t window, bool enabled);
    void apply_fullscreen_if_needed(xcb_window_t window);
    void set_fullscreen_monitors(xcb_window_t window, FullscreenMonitors const& monitors);
    Geometry fullscreen_geometry_for_window(xcb_window_t window) const;
    void iconify_window(xcb_window_t window);
    void deiconify_window(xcb_window_t window, bool focus);
    void kill_window(xcb_window_t window);
    void clear_focus();

    void rearrange_monitor(Monitor& monitor);
    void rearrange_all_monitors();

    // Workspace operations (on focused monitor)
    void switch_workspace(int ws);
    void toggle_workspace();
    void move_window_to_workspace(int ws);

    // Monitor operations
    void focus_monitor(int direction); // -1 = left, +1 = right
    void move_window_to_monitor(int direction);

    void launch_program(std::string const& path);

    // ─────────────────────────────────────────────────────────────────────────
    // Client registry helpers
    // ─────────────────────────────────────────────────────────────────────────
    Client* get_client(xcb_window_t window);
    Client const* get_client(xcb_window_t window) const;
    bool is_managed(xcb_window_t window) const { return clients_.contains(window); }

    // State query helpers (prefer Client, fall back to legacy sets during migration)
    bool is_client_fullscreen(xcb_window_t window) const;
    bool is_client_iconic(xcb_window_t window) const;
    bool is_client_sticky(xcb_window_t window) const;
    bool is_client_above(xcb_window_t window) const;
    bool is_client_below(xcb_window_t window) const;

    // Helpers
    Monitor& focused_monitor() { return monitors_[focused_monitor_]; }
    Monitor const& focused_monitor() const { return monitors_[focused_monitor_]; }
    size_t wrap_monitor_index(int idx) const;
    void warp_to_monitor(Monitor const& monitor);
    void focus_or_fallback(Monitor& monitor);
    Monitor* monitor_containing_window(xcb_window_t window);
    Monitor* monitor_at_point(int16_t x, int16_t y);
    FloatingWindow* find_floating_window(xcb_window_t window);
    FloatingWindow const* find_floating_window(xcb_window_t window) const;
    std::optional<size_t> monitor_index_for_window(xcb_window_t window) const;
    std::optional<size_t> workspace_index_for_window(xcb_window_t window) const;
    std::optional<uint32_t> get_window_desktop(xcb_window_t window) const;
    bool is_sticky_desktop(xcb_window_t window) const;
    std::optional<std::pair<size_t, size_t>> resolve_window_desktop(xcb_window_t window) const;
    std::optional<xcb_window_t> transient_for_window(xcb_window_t window) const;
    bool is_window_visible(xcb_window_t window) const;
    void restack_transients(xcb_window_t parent);
    bool is_override_redirect_window(xcb_window_t window) const;
    bool is_workspace_visible(size_t monitor_idx, size_t workspace_idx) const;
    std::optional<ActiveWindowInfo> get_active_window_info() const;
    BarState build_bar_state(size_t monitor_idx, std::optional<ActiveWindowInfo> const& active_info) const;
    void update_floating_visibility(size_t monitor_idx);
    void update_floating_visibility_all();
    void update_floating_monitor_for_geometry(FloatingWindow& window);
    void apply_floating_geometry(FloatingWindow const& window);
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
    void update_all_bars();
    void update_ewmh_workarea();
    void register_client_window(xcb_window_t window);

    // Dock/strut helpers
    void update_struts();
    void unmanage_dock_window(xcb_window_t window);
    void unmanage_desktop_window(xcb_window_t window);

    // WM-initiated unmap tracking
    void wm_unmap_window(xcb_window_t window);

    // EWMH helpers
    void setup_ewmh();
    void update_ewmh_desktops();
    void update_ewmh_client_list();
    void update_ewmh_current_desktop();
    uint32_t get_ewmh_desktop_index(size_t monitor_idx, size_t workspace_idx) const;
    void switch_to_ewmh_desktop(uint32_t desktop);
    void clear_all_borders();
    xcb_atom_t intern_atom(char const* name) const;
};

} // namespace lwm
