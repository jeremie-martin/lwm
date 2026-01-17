#include "ewmh.hpp"
#include <cstring>

namespace lwm {

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
        ewmh_._NET_WM_DESKTOP,
        ewmh_._NET_DESKTOP_VIEWPORT,
        ewmh_._NET_WM_STATE,
        ewmh_._NET_WM_STATE_DEMANDS_ATTENTION,
        ewmh_._NET_WM_STATE_FULLSCREEN,
        ewmh_._NET_WM_STATE_ABOVE,
        ewmh_._NET_WM_STATE_HIDDEN,
        ewmh_._NET_WM_WINDOW_TYPE,
        ewmh_._NET_WM_WINDOW_TYPE_DOCK,
        ewmh_._NET_WM_WINDOW_TYPE_NORMAL,
        ewmh_._NET_WM_WINDOW_TYPE_DIALOG,
        ewmh_._NET_WM_STRUT,
        ewmh_._NET_WM_STRUT_PARTIAL,
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

void Ewmh::update_client_list(std::vector<xcb_window_t> const& windows)
{
    xcb_ewmh_set_client_list(&ewmh_, 0, windows.size(), const_cast<xcb_window_t*>(windows.data()));
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
        return XCB_ATOM_NONE;

    xcb_atom_t type = (types.atoms_len > 0) ? types.atoms[0] : XCB_ATOM_NONE;
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
    // Don't tile docks, dialogs, or utility windows
    return type != ewmh_._NET_WM_WINDOW_TYPE_DOCK && type != ewmh_._NET_WM_WINDOW_TYPE_DIALOG
        && type != ewmh_._NET_WM_WINDOW_TYPE_UTILITY && type != ewmh_._NET_WM_WINDOW_TYPE_SPLASH
        && type != ewmh_._NET_WM_WINDOW_TYPE_MENU && type != ewmh_._NET_WM_WINDOW_TYPE_DROPDOWN_MENU
        && type != ewmh_._NET_WM_WINDOW_TYPE_POPUP_MENU && type != ewmh_._NET_WM_WINDOW_TYPE_TOOLTIP
        && type != ewmh_._NET_WM_WINDOW_TYPE_NOTIFICATION && type != ewmh_._NET_WM_WINDOW_TYPE_COMBO
        && type != ewmh_._NET_WM_WINDOW_TYPE_DND;
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
