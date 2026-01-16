#pragma once

#include "lwm/bar/bar.hpp"
#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/types.hpp"
#include "lwm/keybind/keybind.hpp"
#include "lwm/layout/layout.hpp"
#include <memory>
#include <vector>

namespace lwm
{

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

    std::vector<Tag> tags_;
    size_t current_tag_;

    void setup_root();
    void handle_event(xcb_generic_event_t const& event);
    void handle_map_request(xcb_map_request_event_t const& e);
    void handle_unmap_notify(xcb_unmap_notify_event_t const& e);
    void handle_destroy_notify(xcb_destroy_notify_event_t const& e);
    void handle_enter_notify(xcb_enter_notify_event_t const& e);
    void handle_key_press(xcb_key_press_event_t const& e);

    void manage_window(xcb_window_t window);
    void unmanage_window(xcb_window_t window);
    void focus_window(xcb_window_t window);
    void kill_window(xcb_window_t window);

    void rearrange_windows();
    void switch_to_tag(int tag);
    void move_window_to_next_tag();
    void launch_program(std::string const& path);

    std::string get_window_name(xcb_window_t window);
};

} // namespace lwm
