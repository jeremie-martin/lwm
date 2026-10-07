#pragma once

#include "connection.hpp"
#include "types.hpp"
#include <array>
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

    // Name the WM's window as the supporting window and advertise the supported
    // atoms, including LWM's extensions.
    void advertise(xcb_window_t check, std::vector<xcb_atom_t> const& extra_supported);

    // Decode the _NET_WM_STATE atoms LWM understands.
    WindowStates states(std::span<xcb_atom_t const> atoms) const;
    // Replace owned values in observed atoms, preserving other parties' atoms.
    std::vector<xcb_atom_t>
    merge_states(std::span<xcb_atom_t const> observed, WindowStates enabled, WindowStates owned) const;

    // First recognized _NET_WM_WINDOW_TYPE atom, or Normal.
    WindowType window_type(std::span<xcb_atom_t const> atoms) const;

    xcb_ewmh_connection_t* get() { return &ewmh_; }
    xcb_ewmh_connection_t* get() const { return &ewmh_; }

private:
    Connection& conn_;
    mutable xcb_ewmh_connection_t ewmh_; // mutable: XCB EWMH API isn't const-correct
    std::array<xcb_atom_t, static_cast<size_t>(WindowState::Count)> state_atoms_{ }; ///< Indexed by WindowState
};

} // namespace lwm
