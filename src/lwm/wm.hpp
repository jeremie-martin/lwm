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
#include <vector>

namespace lwm {

class WindowManager
{
public:
    explicit WindowManager(Config config);

    void run();

private:
    Config config_;
    Connection conn_;
    Ewmh ewmh_;
    KeybindManager keybinds_;
    Layout layout_;
    std::optional<StatusBar> bar_;

    std::vector<Monitor> monitors_;
    std::vector<xcb_window_t> dock_windows_;
    size_t focused_monitor_ = 0;

    void setup_root();
    void detect_monitors();
    void create_fallback_monitor();
    void setup_monitor_bars();

    void handle_event(xcb_generic_event_t const& event);
    void handle_map_request(xcb_map_request_event_t const& e);
    void handle_window_removal(xcb_window_t window);
    void handle_enter_notify(xcb_enter_notify_event_t const& e);
    void handle_key_press(xcb_key_press_event_t const& e);
    void handle_randr_screen_change();

    void manage_window(xcb_window_t window);
    void unmanage_window(xcb_window_t window);
    void focus_window(xcb_window_t window);
    void kill_window(xcb_window_t window);

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
    std::string get_window_name(xcb_window_t window);
    void update_all_bars();

    // Dock/strut helpers
    void update_struts();
    void unmanage_dock_window(xcb_window_t window);

    // EWMH helpers
    void setup_ewmh();
    void update_ewmh_client_list();
    void update_ewmh_current_desktop();
    uint32_t get_ewmh_desktop_index(size_t monitor_idx, size_t workspace_idx) const;
};

} // namespace lwm
