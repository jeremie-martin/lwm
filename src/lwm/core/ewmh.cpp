#include "ewmh.hpp"
#include <algorithm>
#include <cstring>

namespace lwm {
namespace {
struct WindowTypeEntry
{
    xcb_atom_t xcb_ewmh_connection_t::*atom;
    WindowType type;
};
constexpr WindowTypeEntry window_types[] = {
    {       &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_DESKTOP,      WindowType::Desktop },
    {          &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_DOCK,         WindowType::Dock },
    {       &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_TOOLBAR,      WindowType::Toolbar },
    {          &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_MENU,         WindowType::Menu },
    {       &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_UTILITY,      WindowType::Utility },
    {        &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_SPLASH,       WindowType::Splash },
    {        &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_DIALOG,       WindowType::Dialog },
    { &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_DROPDOWN_MENU, WindowType::DropdownMenu },
    {    &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_POPUP_MENU,    WindowType::PopupMenu },
    {       &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_TOOLTIP,      WindowType::Tooltip },
    {  &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_NOTIFICATION, WindowType::Notification },
    {         &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_COMBO,        WindowType::Combo },
    {           &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_DND,          WindowType::Dnd },
    {        &xcb_ewmh_connection_t::_NET_WM_WINDOW_TYPE_NORMAL,       WindowType::Normal },
};

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

    for (auto const& type : window_types) supported.push_back(ewmh_.*type.atom);
    supported.insert(supported.end(), extra_supported.begin(), extra_supported.end());
    xcb_ewmh_set_supported(&ewmh_, 0, supported.size(), supported.data());
}

void Ewmh::set_wm_name(std::string const& name)
{
    xcb_ewmh_set_wm_name(&ewmh_, supporting_window_, name.length(), name.c_str());
}

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

void Ewmh::set_desktop_viewport(std::vector<std::pair<uint32_t, uint32_t>> const& viewports)
{
    std::vector<xcb_ewmh_coordinates_t> coordinates;
    for (auto [x, y] : viewports) coordinates.push_back({ x, y });
    if (!coordinates.empty())
        xcb_ewmh_set_desktop_viewport(&ewmh_, 0, coordinates.size(), coordinates.data());
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

WindowType Ewmh::window_type(std::span<xcb_atom_t const> atoms) const
{
    for (auto atom : atoms)
        if (auto entry = std::ranges::find(window_types, atom, [&](auto const& entry) { return ewmh_.*entry.atom; });
            entry != std::end(window_types))
            return entry->type;
    return WindowType::Normal;
}

} // namespace lwm
