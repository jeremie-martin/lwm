#pragma once

#include "connection.hpp"
#include "types.hpp"
#include <array>
#include <span>
#include <string>
#include <vector>
#include <xcb/xcb_ewmh.h>

namespace lwm {

// Owns the EWMH connection, the WM's advertisement and atom conversions; completion
// writes the properties.
class Ewmh
{
public:
    explicit Ewmh(Connection& conn);
    ~Ewmh();

    Ewmh(Ewmh const&) = delete;
    Ewmh& operator=(Ewmh const&) = delete;

    // Name the WM's window as the supporting window and advertise the supported
    // atoms, including LWM's extensions.
    void advertise(xcb_window_t check, xcb_atom_t window_class);

    // Decode the _NET_WM_STATE atoms LWM understands.
    WindowStates states(std::span<xcb_atom_t const> atoms) const;
    // Replace the atoms LWM understands, preserving other parties' atoms.
    std::vector<xcb_atom_t> merge_states(std::span<xcb_atom_t const> observed, WindowStates enabled) const;

    // First recognized _NET_WM_WINDOW_TYPE atom, or Normal.
    WindowType window_type(std::span<xcb_atom_t const> atoms) const;

    xcb_ewmh_connection_t* get() const { return &ewmh_; }

private:
    Connection& conn_;
    mutable xcb_ewmh_connection_t ewmh_; // mutable: XCB EWMH API isn't const-correct
    std::array<xcb_atom_t, static_cast<size_t>(WindowState::Count)> state_atoms_{ }; ///< Indexed by WindowState
};

} // namespace lwm
