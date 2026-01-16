#pragma once

#include "connection.hpp"
#include "types.hpp"
#include <string>
#include <vector>
#include <xcb/xcb_ewmh.h>

namespace lwm {

class Ewmh
{
public:
    explicit Ewmh(Connection& conn);
    ~Ewmh();

    Ewmh(Ewmh const&) = delete;
    Ewmh& operator=(Ewmh const&) = delete;

    // Root window properties (called once at startup)
    void init_atoms();
    void set_supported_atoms();
    void set_wm_name(std::string const& name);
    void set_number_of_desktops(uint32_t count);
    void set_desktop_names(std::vector<std::string> const& names);

    // Dynamic updates
    void set_current_desktop(uint32_t desktop);
    void set_active_window(xcb_window_t window);
    void set_desktop_viewport(std::vector<Monitor> const& monitors);

    // Per-window properties
    void set_window_desktop(xcb_window_t window, uint32_t desktop);

    // Client list management
    void update_client_list(std::vector<xcb_window_t> const& windows);

    // Urgent hints
    void set_demands_attention(xcb_window_t window, bool urgent);
    bool has_urgent_hint(xcb_window_t window) const;

    // Window type detection
    xcb_atom_t get_window_type(xcb_window_t window) const;
    bool is_dock_window(xcb_window_t window) const;

    // Strut support
    Strut get_window_strut(xcb_window_t window) const;

    xcb_ewmh_connection_t* get() { return &ewmh_; }

private:
    Connection& conn_;
    mutable xcb_ewmh_connection_t ewmh_; // mutable: XCB EWMH API isn't const-correct
    xcb_window_t supporting_window_ = XCB_NONE;

    void create_supporting_window();
};

} // namespace lwm
