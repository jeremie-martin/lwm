#pragma once

#include "connection.hpp"
#include "types.hpp"
#include "workarea.hpp"
#include <map>
#include <string>
#include <vector>
#include <xcb/xcb_ewmh.h>

namespace lwm {

/**
 * @brief Window classification result from EWMH type and properties
 *
 * Classification uses the first recognized `_NET_WM_WINDOW_TYPE` atom and
 * then applies transient status to normal windows.
 */
struct WindowClassification
{
    enum class Kind
    {
        Tiled,
        Floating,
        Dock,
        Desktop,
        Popup
    };

    Kind kind = Kind::Tiled;
    bool skip_taskbar = false;
    bool skip_pager = false;
    bool above = false; // For UTILITY windows
    bool is_transient = false;
};

WindowClassification classify_window_type(WindowType type, bool is_transient);

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
    void set_extra_supported_atoms(std::vector<xcb_atom_t> atoms);
    void set_wm_name(std::string const& name);
    void set_number_of_desktops(uint32_t count);
    void set_desktop_names(std::vector<std::string> const& names);
    void set_workarea(std::vector<Geometry> const& workareas);
    void set_desktop_geometry(uint32_t width, uint32_t height);
    void set_showing_desktop(bool showing);

    // Dynamic updates
    void set_current_desktop(uint32_t desktop);
    void set_active_window(xcb_window_t window);
    void set_desktop_viewport(std::vector<Monitor> const& monitors, int32_t origin_x, int32_t origin_y);

    // Per-window properties
    void set_window_desktop(xcb_window_t window, uint32_t desktop);
    // Accumulate updates until the operation boundary; merge with fresh X state.
    void set_window_state(xcb_window_t window, xcb_atom_t state, bool enabled);
    void flush_window_states();
    bool has_pending_window_states() const { return !state_updates_.empty(); }

    void set_frame_extents(xcb_window_t window, uint32_t left, uint32_t right, uint32_t top, uint32_t bottom);
    void set_allowed_actions(xcb_window_t window, std::vector<xcb_atom_t> const& actions);

    // Client list management
    void update_client_list(std::vector<xcb_window_t> const& windows);
    void update_client_list_stacking(std::vector<xcb_window_t> const& windows);

    // Window type detection and classification
    xcb_atom_t get_window_type(xcb_window_t window) const;
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
    std::vector<xcb_atom_t> extra_supported_atoms_;

    void create_supporting_window();
    std::map<xcb_window_t, std::map<xcb_atom_t, bool>> state_updates_;
};

} // namespace lwm
