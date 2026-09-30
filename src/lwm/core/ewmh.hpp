#pragma once

#include "classification.hpp"
#include "connection.hpp"
#include "types.hpp"
#include "workarea.hpp"
#include <span>
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

    // Create the supporting window and advertise the supported atoms, including LWM's extensions.
    void init_atoms(std::vector<xcb_atom_t> const& extra_supported);
    void set_wm_name(std::string const& name);
    void set_number_of_desktops(uint32_t count);
    void set_desktop_names(std::vector<std::string> const& names);
    void set_workarea(std::vector<Geometry> const& workareas);
    void set_desktop_geometry(uint32_t width, uint32_t height);
    void set_showing_desktop(bool showing);

    // Dynamic updates
    void set_current_desktop(uint32_t desktop);
    void set_active_window(xcb_window_t window);
    void set_desktop_viewport(std::vector<std::pair<uint32_t, uint32_t>> const& viewports);

    // Per-window properties
    void set_window_desktop(xcb_window_t window, uint32_t desktop);
    // Replace the atoms LWM owns in each window's _NET_WM_STATE, preserving
    // atoms owned by other parties. Reads are pipelined across windows.
    void update_window_states(
        std::span<std::pair<xcb_window_t, std::vector<xcb_atom_t>> const> updates,
        std::span<xcb_atom_t const> owned
    );

    void set_frame_extents(xcb_window_t window, uint32_t left, uint32_t right, uint32_t top, uint32_t bottom);
    void set_allowed_actions(xcb_window_t window, std::vector<xcb_atom_t> const& actions);

    // Client list management
    void update_client_list(std::vector<xcb_window_t> const& windows);
    void update_client_list_stacking(std::vector<xcb_window_t> const& windows);

    // First recognized _NET_WM_WINDOW_TYPE, or Normal.
    WindowType get_window_type_enum(xcb_window_t window) const;

    // Strut support
    DockStrut get_window_strut(xcb_window_t window) const;

    void destroy_for_restart();

    xcb_ewmh_connection_t* get() { return &ewmh_; }
    xcb_ewmh_connection_t* get() const { return &ewmh_; }

private:
    Connection& conn_;
    mutable xcb_ewmh_connection_t ewmh_; // mutable: XCB EWMH API isn't const-correct
    xcb_window_t supporting_window_ = XCB_NONE;
    void create_supporting_window();
    xcb_atom_t get_window_type(xcb_window_t window) const;
};

} // namespace lwm
