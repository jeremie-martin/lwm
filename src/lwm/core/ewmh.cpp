#include "ewmh.hpp"
#include "xproperty.hpp"
#include <algorithm>
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

}

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

void Ewmh::destroy_for_restart()
{
    if (supporting_window_ != XCB_NONE)
    {
        xcb_destroy_window(conn_.get(), supporting_window_);
        supporting_window_ = XCB_NONE;
    }
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

    xcb_ewmh_set_supporting_wm_check(&ewmh_, conn_.screen()->root, supporting_window_);
    xcb_ewmh_set_supporting_wm_check(&ewmh_, supporting_window_, supporting_window_);
}


// Keep advertised atoms aligned with the implemented contract in X11.md.
void Ewmh::init_atoms(std::vector<xcb_atom_t> const& extra_supported)
{
    create_supporting_window();
    std::vector<xcb_atom_t> supported = {
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
        ewmh_._NET_WM_STATE_STICKY,
        ewmh_._NET_WM_STATE_MAXIMIZED_VERT,
        ewmh_._NET_WM_STATE_MAXIMIZED_HORZ,

        ewmh_._NET_WM_STATE_MODAL,
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

        ewmh_._NET_WM_ACTION_STICK,
        ewmh_._NET_WM_ACTION_MAXIMIZE_VERT,
        ewmh_._NET_WM_ACTION_MAXIMIZE_HORZ,
        ewmh_._NET_WM_ACTION_MOVE,
        ewmh_._NET_WM_ACTION_RESIZE,
        ewmh_._NET_MOVERESIZE_WINDOW,
        ewmh_._NET_WM_MOVERESIZE,
        ewmh_._NET_SHOWING_DESKTOP,
        ewmh_._NET_RESTACK_WINDOW,
        ewmh_._NET_WM_USER_TIME,
    };

    supported.insert(supported.end(), extra_supported.begin(), extra_supported.end());
    xcb_ewmh_set_supported(&ewmh_, 0, supported.size(), supported.data());
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

void Ewmh::set_desktop_viewport(std::vector<std::pair<uint32_t, uint32_t>> const& viewports)
{
    std::vector<xcb_ewmh_coordinates_t> coordinates;
    for (auto [x, y] : viewports) coordinates.push_back({ x, y });
    if (!coordinates.empty())
        xcb_ewmh_set_desktop_viewport(&ewmh_, 0, coordinates.size(), coordinates.data());
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

void Ewmh::update_window_states(
    std::span<std::pair<xcb_window_t, std::vector<xcb_atom_t>> const> updates,
    std::span<xcb_atom_t const> owned
)
{
    std::vector<xcb_get_property_cookie_t> cookies;
    cookies.reserve(updates.size());
    for (auto const& [window, enabled] : updates) cookies.push_back(xcb_ewmh_get_wm_state(&ewmh_, window));
    for (size_t i = 0; i < updates.size(); ++i)
    {
        auto const& [window, enabled] = updates[i];
        std::vector<xcb_atom_t> atoms;
        xcb_ewmh_get_atoms_reply_t reply{ };
        if (xcb_ewmh_get_wm_state_reply(&ewmh_, cookies[i], &reply, nullptr))
        {
            atoms.assign(reply.atoms, reply.atoms + reply.atoms_len);
            xcb_ewmh_get_atoms_reply_wipe(&reply);
        }
        auto previous = atoms;
        std::erase_if(atoms, [&](xcb_atom_t atom) { return std::ranges::find(owned, atom) != owned.end(); });
        atoms.insert(atoms.end(), enabled.begin(), enabled.end());
        if (atoms == previous)
            continue;
        if (atoms.empty())
            xcb_delete_property(conn_.get(), window, ewmh_._NET_WM_STATE);
        else
            xcb_ewmh_set_wm_state(&ewmh_, window, atoms.size(), atoms.data());
    }
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

WindowType Ewmh::get_window_type_enum(xcb_window_t window) const
{
    xcb_atom_t type = get_window_type(window);

    struct Entry { xcb_atom_t atom; WindowType wtype; };
    Entry const table[] = {
        { ewmh_._NET_WM_WINDOW_TYPE_DESKTOP,      WindowType::Desktop },
        { ewmh_._NET_WM_WINDOW_TYPE_DOCK,          WindowType::Dock },
        { ewmh_._NET_WM_WINDOW_TYPE_TOOLBAR,       WindowType::Toolbar },
        { ewmh_._NET_WM_WINDOW_TYPE_MENU,          WindowType::Menu },
        { ewmh_._NET_WM_WINDOW_TYPE_UTILITY,       WindowType::Utility },
        { ewmh_._NET_WM_WINDOW_TYPE_SPLASH,        WindowType::Splash },
        { ewmh_._NET_WM_WINDOW_TYPE_DIALOG,        WindowType::Dialog },
        { ewmh_._NET_WM_WINDOW_TYPE_DROPDOWN_MENU, WindowType::DropdownMenu },
        { ewmh_._NET_WM_WINDOW_TYPE_POPUP_MENU,    WindowType::PopupMenu },
        { ewmh_._NET_WM_WINDOW_TYPE_TOOLTIP,       WindowType::Tooltip },
        { ewmh_._NET_WM_WINDOW_TYPE_NOTIFICATION,  WindowType::Notification },
        { ewmh_._NET_WM_WINDOW_TYPE_COMBO,         WindowType::Combo },
        { ewmh_._NET_WM_WINDOW_TYPE_DND,           WindowType::Dnd },
    };

    for (auto const& e : table)
        if (type == e.atom)
            return e.wtype;

    return WindowType::Normal;
}

DockStrut Ewmh::get_window_strut(xcb_window_t window) const
{
    auto partial_reply = xproperty::read(conn_.get(), window, ewmh_._NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 12);
    auto partial = xproperty::words(partial_reply, XCB_ATOM_CARDINAL);
    if (partial.size() == 12)
        return {
            { partial[0],  partial[4],  partial[5] },
            { partial[1],  partial[6],  partial[7] },
            { partial[2],  partial[8],  partial[9] },
            { partial[3], partial[10], partial[11] }
        };
    auto legacy_reply = xproperty::read(conn_.get(), window, ewmh_._NET_WM_STRUT, XCB_ATOM_CARDINAL, 4);
    auto legacy = xproperty::words(legacy_reply, XCB_ATOM_CARDINAL);
    if (legacy.size() == 4)
        return { { legacy[0] }, { legacy[1] }, { legacy[2] }, { legacy[3] } };
    return { };
}

}
