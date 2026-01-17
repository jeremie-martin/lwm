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
    };

    struct DragState
    {
        bool active = false;
        bool resizing = false;
        xcb_window_t window = XCB_NONE;
        int16_t start_root_x = 0;
        int16_t start_root_y = 0;
        Geometry start_geometry;
    };

    struct FullscreenMonitors
    {
        uint32_t top = 0;
        uint32_t bottom = 0;
        uint32_t left = 0;
        uint32_t right = 0;
    };

    Config config_;
    Connection conn_;
    Ewmh ewmh_;
    KeybindManager keybinds_;
    Layout layout_;
    std::optional<StatusBar> bar_;

    std::vector<Monitor> monitors_;
    std::vector<xcb_window_t> dock_windows_;
    std::vector<FloatingWindow> floating_windows_;
    std::unordered_set<xcb_window_t> wm_unmapped_windows_;
    std::unordered_set<xcb_window_t> fullscreen_windows_;
    std::unordered_set<xcb_window_t> above_windows_;
    std::unordered_set<xcb_window_t> below_windows_;
    std::unordered_set<xcb_window_t> iconic_windows_;
    std::unordered_set<xcb_window_t> skip_taskbar_windows_;
    std::unordered_set<xcb_window_t> skip_pager_windows_;
    bool showing_desktop_ = false;
    std::unordered_map<xcb_window_t, Geometry> fullscreen_restore_;
    std::unordered_map<xcb_window_t, FullscreenMonitors> fullscreen_monitors_;
    std::unordered_map<xcb_window_t, xcb_sync_counter_t> sync_counters_;
    std::unordered_map<xcb_window_t, uint64_t> sync_values_;
    std::unordered_map<xcb_window_t, std::chrono::steady_clock::time_point> pending_kills_;
    std::unordered_map<xcb_window_t, std::chrono::steady_clock::time_point> pending_pings_;
    std::unordered_map<xcb_window_t, uint32_t> user_times_;
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
    bool suppress_focus_ = false;
    uint32_t last_event_time_ = XCB_CURRENT_TIME;
    DragState drag_state_;

    void create_wm_window();
    void setup_root();
    void grab_buttons();
    void claim_wm_ownership();
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

    void manage_window(xcb_window_t window);
    void manage_floating_window(xcb_window_t window);
    void unmanage_window(xcb_window_t window);
    void unmanage_floating_window(xcb_window_t window);
    void focus_window(xcb_window_t window);
    void focus_floating_window(xcb_window_t window);
    void set_fullscreen(xcb_window_t window, bool enabled);
    void set_window_above(xcb_window_t window, bool enabled);
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
    std::optional<xcb_window_t> transient_for_window(xcb_window_t window) const;
    bool is_override_redirect_window(xcb_window_t window) const;
    void update_floating_visibility(size_t monitor_idx);
    void update_floating_visibility_all();
    void update_floating_monitor_for_geometry(FloatingWindow& window);
    void apply_floating_geometry(FloatingWindow const& window);
    void send_configure_notify(xcb_window_t window);
    void begin_drag(xcb_window_t window, bool resize, int16_t root_x, int16_t root_y);
    void update_drag(int16_t root_x, int16_t root_y);
    void end_drag();
    bool supports_protocol(xcb_window_t window, xcb_atom_t protocol) const;
    bool should_set_input_focus(xcb_window_t window) const;
    void send_wm_take_focus(xcb_window_t window, uint32_t timestamp);
    void send_wm_ping(xcb_window_t window, uint32_t timestamp);
    void send_sync_request(xcb_window_t window, uint32_t timestamp);
    void update_sync_state(xcb_window_t window);
    void update_fullscreen_monitor_state(xcb_window_t window);
    void update_focused_monitor_at_point(int16_t x, int16_t y);
    std::string get_window_name(xcb_window_t window);
    std::pair<std::string, std::string> get_wm_class(xcb_window_t window);
    uint32_t get_user_time(xcb_window_t window);
    void update_window_title(xcb_window_t window);
    void update_all_bars();
    void update_ewmh_workarea();

    // Dock/strut helpers
    void update_struts();
    void unmanage_dock_window(xcb_window_t window);

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
