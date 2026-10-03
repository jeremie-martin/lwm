#pragma once

#include "connection.hpp"
#include "types.hpp"
#include <span>
#include <string>
#include <vector>
#include <xcb/xcb_ewmh.h>

namespace lwm {

// Owns EWMH resources and protocol conversions; simple property writes use XCB directly.
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
    void set_desktop_names(std::vector<std::string> const& names);
    void set_workarea(std::vector<Geometry> const& workareas);
    void set_desktop_viewport(std::vector<std::pair<uint32_t, uint32_t>> const& viewports);

    // Replace the atoms LWM owns in each window's _NET_WM_STATE, preserving
    // atoms owned by other parties. Reads are pipelined across windows.
    void update_window_states(
        std::span<std::pair<xcb_window_t, std::vector<xcb_atom_t>> const> updates,
        std::span<xcb_atom_t const> owned
    );

    // First recognized _NET_WM_WINDOW_TYPE atom, or Normal.
    WindowType window_type(std::span<xcb_atom_t const> atoms) const;

    void destroy_for_restart();

    xcb_ewmh_connection_t* get() { return &ewmh_; }
    xcb_ewmh_connection_t* get() const { return &ewmh_; }

private:
    Connection& conn_;
    mutable xcb_ewmh_connection_t ewmh_; // mutable: XCB EWMH API isn't const-correct
    xcb_window_t supporting_window_ = XCB_NONE;
    void create_supporting_window();
};

} // namespace lwm
