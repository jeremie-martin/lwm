#include "ewmh.hpp"
#include <cstring>

namespace lwm {

namespace {

bool is_known_window_type(xcb_ewmh_connection_t const* ewmh, xcb_atom_t type)
{
    return type == ewmh->_NET_WM_WINDOW_TYPE_DESKTOP || type == ewmh->_NET_WM_WINDOW_TYPE_DOCK
        || type == ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR || type == ewmh->_NET_WM_WINDOW_TYPE_MENU
        || type == ewmh->_NET_WM_WINDOW_TYPE_UTILITY || type == ewmh->_NET_WM_WINDOW_TYPE_SPLASH
        || type == ewmh->_NET_WM_WINDOW_TYPE_DIALOG || type == ewmh->_NET_WM_WINDOW_TYPE_DROPDOWN_MENU
        || type == ewmh->_NET_WM_WINDOW_TYPE_POPUP_MENU || type == ewmh->_NET_WM_WINDOW_TYPE_TOOLTIP
        || type == ewmh->_NET_WM_WINDOW_TYPE_NOTIFICATION || type == ewmh->_NET_WM_WINDOW_TYPE_COMBO
        || type == ewmh->_NET_WM_WINDOW_TYPE_DND || type == ewmh->_NET_WM_WINDOW_TYPE_NORMAL;
}

} // namespace

Ewmh::Ewmh(Connection& conn)
    : conn_(conn)
{
    xcb_intern_atom_cookie_t* cookies = xcb_ewmh_init_atoms(conn_.get(), &ewmh_);
    if (!xcb_ewmh_init_atoms_replies(&ewmh_, cookies, nullptr))
    {
        throw std::runtime_error("Failed to initialize EWMH atoms");
    }
}

Ewmh::~Ewmh()
{
    if (supporting_window_ != XCB_NONE)
    {
        xcb_destroy_window(conn_.get(), supporting_window_);
    }
    xcb_ewmh_connection_wipe(&ewmh_);
}

void Ewmh::init_atoms()
{
    create_supporting_window();
    set_supported_atoms();
}

void Ewmh::create_supporting_window()
{
    supporting_window_ = xcb_generate_id(conn_.get());
    xcb_create_window(
        conn_.get(),
        XCB_COPY_FROM_PARENT,
        supporting_window_,
        conn_.screen()->root,
        -1,
        -1,
        1,
        1,
        0,
        XCB_WINDOW_CLASS_INPUT_ONLY,
        XCB_COPY_FROM_PARENT,
        0,
        nullptr
    );

    // Set _NET_SUPPORTING_WM_CHECK on both root and supporting window
    xcb_ewmh_set_supporting_wm_check(&ewmh_, conn_.screen()->root, supporting_window_);
    xcb_ewmh_set_supporting_wm_check(&ewmh_, supporting_window_, supporting_window_);
}

void Ewmh::set_supported_atoms()
{
    xcb_atom_t supported[] = {
        ewmh_._NET_SUPPORTED,
        ewmh_._NET_SUPPORTING_WM_CHECK,
        ewmh_._NET_WM_NAME,
        ewmh_._NET_NUMBER_OF_DESKTOPS,
        ewmh_._NET_DESKTOP_NAMES,
        ewmh_._NET_CURRENT_DESKTOP,
        ewmh_._NET_ACTIVE_WINDOW,
        ewmh_._NET_CLIENT_LIST,
        ewmh_._NET_CLIENT_LIST_STACKING,
        ewmh_._NET_WM_DESKTOP,
        ewmh_._NET_DESKTOP_VIEWPORT,
        ewmh_._NET_DESKTOP_GEOMETRY,
        ewmh_._NET_WORKAREA,
        ewmh_._NET_WM_STATE,
        ewmh_._NET_WM_STATE_DEMANDS_ATTENTION,
        ewmh_._NET_WM_STATE_FULLSCREEN,
        ewmh_._NET_WM_STATE_ABOVE,
        ewmh_._NET_WM_STATE_BELOW,
        ewmh_._NET_WM_STATE_HIDDEN,
        ewmh_._NET_WM_STATE_SKIP_TASKBAR,
        ewmh_._NET_WM_STATE_SKIP_PAGER,
        ewmh_._NET_WM_PING,
        ewmh_._NET_WM_SYNC_REQUEST,
        ewmh_._NET_WM_SYNC_REQUEST_COUNTER,
        ewmh_._NET_CLOSE_WINDOW,
        ewmh_._NET_WM_FULLSCREEN_MONITORS,
        ewmh_._NET_WM_WINDOW_TYPE,
        ewmh_._NET_WM_WINDOW_TYPE_DESKTOP,
        ewmh_._NET_WM_WINDOW_TYPE_DOCK,
        ewmh_._NET_WM_WINDOW_TYPE_TOOLBAR,
        ewmh_._NET_WM_WINDOW_TYPE_MENU,
        ewmh_._NET_WM_WINDOW_TYPE_UTILITY,
        ewmh_._NET_WM_WINDOW_TYPE_SPLASH,
        ewmh_._NET_WM_WINDOW_TYPE_DIALOG,
        ewmh_._NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
        ewmh_._NET_WM_WINDOW_TYPE_POPUP_MENU,
        ewmh_._NET_WM_WINDOW_TYPE_TOOLTIP,
        ewmh_._NET_WM_WINDOW_TYPE_NOTIFICATION,
        ewmh_._NET_WM_WINDOW_TYPE_COMBO,
        ewmh_._NET_WM_WINDOW_TYPE_DND,
        ewmh_._NET_WM_WINDOW_TYPE_NORMAL,
        ewmh_._NET_WM_STRUT,
        ewmh_._NET_WM_STRUT_PARTIAL,
        ewmh_._NET_FRAME_EXTENTS,
        ewmh_._NET_REQUEST_FRAME_EXTENTS,
        ewmh_._NET_WM_ALLOWED_ACTIONS,
        ewmh_._NET_WM_ACTION_CLOSE,
        ewmh_._NET_WM_ACTION_FULLSCREEN,
        ewmh_._NET_WM_ACTION_CHANGE_DESKTOP,
        ewmh_._NET_WM_ACTION_ABOVE,
        ewmh_._NET_WM_ACTION_BELOW,
        ewmh_._NET_WM_ACTION_MINIMIZE,
        ewmh_._NET_WM_ACTION_MOVE,
        ewmh_._NET_WM_ACTION_RESIZE,
        ewmh_._NET_MOVERESIZE_WINDOW,
        ewmh_._NET_WM_MOVERESIZE,
        ewmh_._NET_SHOWING_DESKTOP,
        ewmh_._NET_RESTACK_WINDOW,
    };

    xcb_ewmh_set_supported(&ewmh_, 0, sizeof(supported) / sizeof(supported[0]), supported);
}

void Ewmh::set_wm_name(std::string const& name)
{
    xcb_ewmh_set_wm_name(&ewmh_, supporting_window_, name.length(), name.c_str());
}

void Ewmh::set_number_of_desktops(uint32_t count) { xcb_ewmh_set_number_of_desktops(&ewmh_, 0, count); }

void Ewmh::set_desktop_names(std::vector<std::string> const& names)
{
    std::string combined;
    for (auto const& name : names)
    {
        combined += name;
        combined += '\0';
    }
    xcb_ewmh_set_desktop_names(&ewmh_, 0, combined.length(), combined.c_str());
}

void Ewmh::set_workarea(std::vector<Geometry> const& workareas)
{
    std::vector<xcb_ewmh_geometry_t> areas;
    areas.reserve(workareas.size());
    for (auto const& area : workareas)
    {
        areas.push_back({ static_cast<uint32_t>(area.x), static_cast<uint32_t>(area.y), area.width, area.height });
    }

    if (!areas.empty())
    {
        xcb_ewmh_set_workarea(&ewmh_, 0, areas.size(), areas.data());
    }
}

void Ewmh::set_desktop_geometry(uint32_t width, uint32_t height)
{
    xcb_ewmh_set_desktop_geometry(&ewmh_, 0, width, height);
}

void Ewmh::set_showing_desktop(bool showing) { xcb_ewmh_set_showing_desktop(&ewmh_, 0, showing ? 1 : 0); }

void Ewmh::set_current_desktop(uint32_t desktop) { xcb_ewmh_set_current_desktop(&ewmh_, 0, desktop); }

void Ewmh::set_active_window(xcb_window_t window) { xcb_ewmh_set_active_window(&ewmh_, 0, window); }

void Ewmh::set_desktop_viewport(std::vector<Monitor> const& monitors)
{
    // For multi-monitor with per-monitor workspaces, we set viewport coordinates
    // Each workspace maps to a monitor's position
    std::vector<xcb_ewmh_coordinates_t> viewports;

    for (auto const& monitor : monitors)
    {
        for (size_t i = 0; i < monitor.workspaces.size(); ++i)
        {
            viewports.push_back({ static_cast<uint32_t>(monitor.x), static_cast<uint32_t>(monitor.y) });
        }
    }

    if (!viewports.empty())
    {
        xcb_ewmh_set_desktop_viewport(&ewmh_, 0, viewports.size(), viewports.data());
    }
}

void Ewmh::set_window_desktop(xcb_window_t window, uint32_t desktop)
{
    xcb_ewmh_set_wm_desktop(&ewmh_, window, desktop);
}

void Ewmh::set_frame_extents(xcb_window_t window, uint32_t left, uint32_t right, uint32_t top, uint32_t bottom)
{
    xcb_ewmh_set_frame_extents(&ewmh_, window, left, right, top, bottom);
}

void Ewmh::set_allowed_actions(xcb_window_t window, std::vector<xcb_atom_t> const& actions)
{
    xcb_ewmh_set_wm_allowed_actions(&ewmh_, window, actions.size(), const_cast<xcb_atom_t*>(actions.data()));
}

void Ewmh::update_client_list(std::vector<xcb_window_t> const& windows)
{
    xcb_ewmh_set_client_list(&ewmh_, 0, windows.size(), const_cast<xcb_window_t*>(windows.data()));
}

void Ewmh::update_client_list_stacking(std::vector<xcb_window_t> const& windows)
{
    xcb_ewmh_set_client_list_stacking(&ewmh_, 0, windows.size(), const_cast<xcb_window_t*>(windows.data()));
}

void Ewmh::set_window_state(xcb_window_t window, xcb_atom_t state, bool enabled)
{
    xcb_ewmh_get_atoms_reply_t current_state;
    bool has_current =
        xcb_ewmh_get_wm_state_reply(&ewmh_, xcb_ewmh_get_wm_state(&ewmh_, window), &current_state, nullptr);

    std::vector<xcb_atom_t> new_state;

    if (has_current)
    {
        for (uint32_t i = 0; i < current_state.atoms_len; ++i)
        {
            if (current_state.atoms[i] != state)
            {
                new_state.push_back(current_state.atoms[i]);
            }
        }
        xcb_ewmh_get_atoms_reply_wipe(&current_state);
    }

    if (enabled)
    {
        new_state.push_back(state);
    }

    if (new_state.empty())
    {
        xcb_delete_property(conn_.get(), window, ewmh_._NET_WM_STATE);
    }
    else
    {
        xcb_ewmh_set_wm_state(&ewmh_, window, new_state.size(), new_state.data());
    }
}

bool Ewmh::has_window_state(xcb_window_t window, xcb_atom_t state) const
{
    xcb_ewmh_get_atoms_reply_t current_state;
    if (!xcb_ewmh_get_wm_state_reply(&ewmh_, xcb_ewmh_get_wm_state(&ewmh_, window), &current_state, nullptr))
        return false;

    bool found = false;
    for (uint32_t i = 0; i < current_state.atoms_len; ++i)
    {
        if (current_state.atoms[i] == state)
        {
            found = true;
            break;
        }
    }

    xcb_ewmh_get_atoms_reply_wipe(&current_state);
    return found;
}

void Ewmh::set_demands_attention(xcb_window_t window, bool urgent)
{
    set_window_state(window, ewmh_._NET_WM_STATE_DEMANDS_ATTENTION, urgent);
}

bool Ewmh::has_urgent_hint(xcb_window_t window) const
{
    xcb_ewmh_get_atoms_reply_t state;
    if (!xcb_ewmh_get_wm_state_reply(&ewmh_, xcb_ewmh_get_wm_state(&ewmh_, window), &state, nullptr))
        return false;

    bool urgent = false;
    for (uint32_t i = 0; i < state.atoms_len; ++i)
    {
        if (state.atoms[i] == ewmh_._NET_WM_STATE_DEMANDS_ATTENTION)
        {
            urgent = true;
            break;
        }
    }

    xcb_ewmh_get_atoms_reply_wipe(&state);
    return urgent;
}

xcb_atom_t Ewmh::get_window_type(xcb_window_t window) const
{
    xcb_ewmh_get_atoms_reply_t types;
    if (!xcb_ewmh_get_wm_window_type_reply(&ewmh_, xcb_ewmh_get_wm_window_type(&ewmh_, window), &types, nullptr))
        return ewmh_._NET_WM_WINDOW_TYPE_NORMAL;

    xcb_atom_t type = ewmh_._NET_WM_WINDOW_TYPE_NORMAL;
    for (uint32_t i = 0; i < types.atoms_len; ++i)
    {
        if (is_known_window_type(&ewmh_, types.atoms[i]))
        {
            type = types.atoms[i];
            break;
        }
    }
    xcb_ewmh_get_atoms_reply_wipe(&types);
    return type;
}

bool Ewmh::is_dock_window(xcb_window_t window) const
{
    return get_window_type(window) == ewmh_._NET_WM_WINDOW_TYPE_DOCK;
}

bool Ewmh::is_dialog_window(xcb_window_t window) const
{
    return get_window_type(window) == ewmh_._NET_WM_WINDOW_TYPE_DIALOG;
}

bool Ewmh::should_tile_window(xcb_window_t window) const
{
    xcb_atom_t type = get_window_type(window);
    return type == ewmh_._NET_WM_WINDOW_TYPE_NORMAL;
}

Strut Ewmh::get_window_strut(xcb_window_t window) const
{
    Strut strut;

    // Try _NET_WM_STRUT_PARTIAL first (more detailed)
    xcb_ewmh_wm_strut_partial_t partial;
    if (xcb_ewmh_get_wm_strut_partial_reply(&ewmh_, xcb_ewmh_get_wm_strut_partial(&ewmh_, window), &partial, nullptr))
    {
        strut.left = partial.left;
        strut.right = partial.right;
        strut.top = partial.top;
        strut.bottom = partial.bottom;
        return strut;
    }

    // Fall back to _NET_WM_STRUT
    xcb_ewmh_get_extents_reply_t extents;
    if (xcb_ewmh_get_wm_strut_reply(&ewmh_, xcb_ewmh_get_wm_strut(&ewmh_, window), &extents, nullptr))
    {
        strut.left = extents.left;
        strut.right = extents.right;
        strut.top = extents.top;
        strut.bottom = extents.bottom;
    }

    return strut;
}

} // namespace lwm
