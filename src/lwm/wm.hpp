#pragma once

#include "lwm/bar/bar.hpp"
#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/types.hpp"
#include "lwm/keybind/keybind.hpp"
#include "lwm/layout/layout.hpp"
#include <memory>
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
    KeybindManager keybinds_;
    Layout layout_;
    StatusBar bar_;

    std::vector<Monitor> monitors_;
    size_t focused_monitor_ = 0;

    void setup_root();
    void detect_monitors();
    void create_fallback_monitor();
    void setup_monitor_bars();

    void handle_event(xcb_generic_event_t const& event);
    void handle_map_request(xcb_map_request_event_t const& e);
    void handle_unmap_notify(xcb_unmap_notify_event_t const& e);
    void handle_destroy_notify(xcb_destroy_notify_event_t const& e);
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
};

} // namespace lwm
