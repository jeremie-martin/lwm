#pragma once

#include "lwm/bar/bar.hpp"
#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/ewmh.hpp"
#include "lwm/core/types.hpp"
#include "lwm/keybind/keybind.hpp"
#include "lwm/layout/layout.hpp"
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

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
    xcb_window_t active_window_ = XCB_NONE;
    size_t focused_monitor_ = 0; // Active monitor index (window focus tracked separately).
    xcb_atom_t wm_transient_for_ = XCB_NONE;
    DragState drag_state_;

    void setup_root();
    void detect_monitors();
    void create_fallback_monitor();
    void setup_monitor_bars();
    void init_monitor_workspaces(Monitor& monitor);

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
    void handle_randr_screen_change();

    void manage_window(xcb_window_t window);
    void manage_floating_window(xcb_window_t window);
    void unmanage_window(xcb_window_t window);
    void unmanage_floating_window(xcb_window_t window);
    void focus_window(xcb_window_t window);
    void focus_floating_window(xcb_window_t window);
    void kill_window(xcb_window_t window);
    void clear_focus();

    void rearrange_monitor(Monitor& monitor);
    void rearrange_all_monitors();

    // Workspace operations (on focused monitor)
    void switch_workspace(int ws);
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
    void begin_drag(xcb_window_t window, bool resize, int16_t root_x, int16_t root_y);
    void update_drag(int16_t root_x, int16_t root_y);
    void end_drag();
    void update_focused_monitor_at_point(int16_t x, int16_t y);
    std::string get_window_name(xcb_window_t window);
    void update_all_bars();

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
