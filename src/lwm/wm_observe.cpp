// X reads. Every observation sends its requests before collecting any reply,
// so admission costs one round trip per batch; single-property updates reuse
// the same decoders.

#include "lwm/core/xproperty.hpp"
#include "wm.hpp"
#include <algorithm>
#include <xcb/xcb_icccm.h>

namespace lwm {

namespace {

xcb_get_property_cookie_t request(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint32_t limit)
{
    return xcb_get_property(conn, 0, window, property, type, 0, limit);
}

std::optional<uint32_t> scalar(xcb_connection_t* conn, xcb_get_property_cookie_t cookie, xcb_atom_t type)
{
    auto reply = xproperty::receive(conn, cookie);
    auto values = xproperty::words(reply, type);
    return values.size() == 1 ? std::optional{ values.front() } : std::nullopt;
}

// Titles are display metadata: a bounded prefix of either encoding.
std::optional<std::string> text(xcb_connection_t* conn, xcb_get_property_cookie_t cookie, xcb_atom_t type)
{
    auto reply = xproperty::receive(conn, cookie);
    if (!reply || reply->type != type || reply->format != 8 || xcb_get_property_value_length(reply.get()) == 0)
        return std::nullopt;
    return std::string(static_cast<char const*>(xcb_get_property_value(reply.get())), xcb_get_property_value_length(reply.get()));
}

constexpr uint32_t kTitleLimit = 1024;

struct NameCookies
{
    xcb_get_property_cookie_t net, legacy;
};

std::string name(xcb_connection_t* conn, NameCookies cookies, xcb_atom_t utf8)
{
    auto net = text(conn, cookies.net, utf8);
    auto legacy = text(conn, cookies.legacy, XCB_ATOM_STRING);
    return net ? *net : legacy ? *legacy : "Unnamed";
}

std::pair<std::string, std::string> wm_class(xcb_connection_t* conn, xcb_get_property_cookie_t cookie)
{
    xcb_icccm_get_wm_class_reply_t reply;
    if (!xcb_icccm_get_wm_class_reply(conn, cookie, &reply, nullptr))
        return { };
    std::pair<std::string, std::string> result{ reply.instance_name ? reply.instance_name : "",
                                                reply.class_name ? reply.class_name : "" };
    xcb_icccm_get_wm_class_reply_wipe(&reply);
    return result;
}

xcb_window_t window_value(xcb_connection_t* conn, xcb_get_property_cookie_t cookie)
{
    return scalar(conn, cookie, XCB_ATOM_WINDOW).value_or(XCB_NONE);
}

struct Hints
{
    bool accepts_input = true; // Missing hints mean input is accepted
    bool initially_iconic = false;
    bool urgent = false;
};

Hints hints(xcb_connection_t* conn, xcb_get_property_cookie_t cookie)
{
    xcb_icccm_wm_hints_t reply{ };
    if (!xcb_icccm_get_wm_hints_reply(conn, cookie, &reply, nullptr))
        return { };
    return { !(reply.flags & XCB_ICCCM_WM_HINT_INPUT) || reply.input,
             (reply.flags & XCB_ICCCM_WM_HINT_STATE) && reply.initial_state == XCB_ICCCM_WM_STATE_ICONIC,
             (reply.flags & XUrgencyHint) != 0 };
}

// Nonpositive sizes keep the current extent; oversized ones saturate.
SizeHints size_hints(xcb_connection_t* conn, xcb_get_property_cookie_t cookie)
{
    SizeHints result;
    xcb_size_hints_t reply;
    if (!xcb_icccm_get_wm_normal_hints_reply(conn, cookie, &reply, nullptr))
        return result;
    std::pair position{ geometry_coordinate(reply.x), geometry_coordinate(reply.y) };
    if (reply.flags & XCB_ICCCM_SIZE_HINT_US_POSITION)
        result.user_position = position;
    if (reply.flags & XCB_ICCCM_SIZE_HINT_P_POSITION)
        result.program_position = position;
    if (reply.flags & (XCB_ICCCM_SIZE_HINT_US_SIZE | XCB_ICCCM_SIZE_HINT_P_SIZE))
    {
        if (reply.width > 0)
            result.width = geometry_extent(reply.width);
        if (reply.height > 0)
            result.height = geometry_extent(reply.height);
    }
    return result;
}

std::vector<xcb_atom_t> protocols(xcb_connection_t* conn, xcb_get_property_cookie_t cookie)
{
    xcb_icccm_get_wm_protocols_reply_t reply;
    if (!xcb_icccm_get_wm_protocols_reply(conn, cookie, &reply, nullptr))
        return { };
    std::vector<xcb_atom_t> result(reply.atoms, reply.atoms + reply.atoms_len);
    xcb_icccm_get_wm_protocols_reply_wipe(&reply);
    return result;
}

struct StrutCookies
{
    xcb_get_property_cookie_t partial, legacy;
};

DockStrut strut(xcb_connection_t* conn, StrutCookies cookies)
{
    auto partial_reply = xproperty::receive(conn, cookies.partial);
    auto legacy_reply = xproperty::receive(conn, cookies.legacy);
    auto partial = xproperty::words(partial_reply, XCB_ATOM_CARDINAL);
    if (partial.size() == 12)
        return {
            { partial[0], partial[4], partial[5] },
            { partial[1], partial[6], partial[7] },
            { partial[2], partial[8], partial[9] },
            { partial[3], partial[10], partial[11] },
        };
    auto legacy = xproperty::words(legacy_reply, XCB_ATOM_CARDINAL);
    if (legacy.size() == 4)
        return { { legacy[0] }, { legacy[1] }, { legacy[2] }, { legacy[3] } };
    return { };
}

} // namespace

// Two pipelined stages: identity and role first, then only what the role needs.
std::vector<WindowManager::Observed> WindowManager::observe(std::span<xcb_window_t const> windows, bool adopting)
{
    auto* c = conn_.get();
    auto* e = ewmh_.get();
    struct Identity
    {
        xcb_get_window_attributes_cookie_t attributes;
        xcb_get_property_cookie_t type, transient;
    };
    std::vector<Identity> identities;
    identities.reserve(windows.size());
    for (auto window : windows)
        identities.push_back({ xcb_get_window_attributes(c, window),
                               xcb_ewmh_get_wm_window_type(e, window),
                               request(c, window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 1) });
    std::vector<Observed> result(windows.size());
    for (size_t i = 0; i < windows.size(); ++i)
    {
        auto& observed = result[i];
        auto& w = observed.window;
        w.id = windows[i];
        if (auto* attributes = xcb_get_window_attributes_reply(c, identities[i].attributes, nullptr))
        {
            observed.manageable =
                !attributes->override_redirect && (!adopting || attributes->map_state == XCB_MAP_STATE_VIEWABLE);
            free(attributes);
        }
        w.type = window_type(identities[i].type);
        w.transient_for = window_value(c, identities[i].transient);
        observed.role = State::role(w.id, w.type, w.transient_for != XCB_NONE, handoff_ ? &*handoff_ : nullptr);
    }

    struct Properties
    {
        xcb_get_geometry_cookie_t geometry;
        NameCookies name;
        xcb_get_property_cookie_t wm_class, desktop, state, hints, normal_hints, protocols, time_window, time,
            fullscreen_monitors, sync_counter;
    };
    std::vector<std::optional<Properties>> properties(windows.size());
    std::vector<std::optional<StrutCookies>> struts(windows.size());
    for (size_t i = 0; i < windows.size(); ++i)
    {
        auto window = windows[i];
        if (!result[i].manageable)
            continue;
        // Subscribe before reading, so a later property change cannot be missed.
        auto role = result[i].role;
        if (role != WindowRole::Popup)
        {
            uint32_t mask = role == WindowRole::Desktop ? XCB_EVENT_MASK_PROPERTY_CHANGE : kManagedWindowEventMask;
            xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &mask);
        }
        if (role == WindowRole::Dock)
            struts[i] = StrutCookies{ request(c, window, e->_NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 12),
                                      request(c, window, e->_NET_WM_STRUT, XCB_ATOM_CARDINAL, 4) };
        if (result[i].role == WindowRole::Client)
            properties[i] = Properties{
                xcb_get_geometry(c, window),
                { request(c, window, e->_NET_WM_NAME, e->UTF8_STRING, kTitleLimit),
                  request(c, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, kTitleLimit) },
                xcb_icccm_get_wm_class(c, window),
                xcb_ewmh_get_wm_desktop(e, window),
                xcb_ewmh_get_wm_state(e, window),
                xcb_icccm_get_wm_hints(c, window),
                xcb_icccm_get_wm_normal_hints(c, window),
                xcb_icccm_get_wm_protocols(c, window, e->WM_PROTOCOLS),
                request(c, window, e->_NET_WM_USER_TIME_WINDOW, XCB_ATOM_WINDOW, 1),
                request(c, window, e->_NET_WM_USER_TIME, XCB_ATOM_CARDINAL, 1),
                xcb_ewmh_get_wm_fullscreen_monitors(e, window),
                request(c, window, e->_NET_WM_SYNC_REQUEST_COUNTER, XCB_ATOM_CARDINAL, 2),
            };
    }
    for (size_t i = 0; i < windows.size(); ++i)
    {
        auto& observed = result[i];
        auto& w = observed.window;
        if (struts[i])
            w.strut = strut(c, *struts[i]);
        if (!properties[i])
            continue;
        auto const& cookie = *properties[i];
        if (auto* geometry = xcb_get_geometry_reply(c, cookie.geometry, nullptr))
        {
            w.geometry = Geometry{ geometry->x, geometry->y, geometry->width, geometry->height };
            free(geometry);
        }
        w.name = name(c, cookie.name, e->UTF8_STRING);
        std::tie(w.wm_class_name, w.wm_class) = wm_class(c, cookie.wm_class);
        uint32_t desktop = 0;
        if (xcb_ewmh_get_wm_desktop_reply(e, cookie.desktop, &desktop, nullptr))
            w.desktop = desktop;
        xcb_ewmh_get_atoms_reply_t states;
        if (xcb_ewmh_get_wm_state_reply(e, cookie.state, &states, nullptr))
        {
            w.states = ewmh_.states({ states.atoms, states.atoms_len });
            xcb_ewmh_get_atoms_reply_wipe(&states);
        }
        auto observed_hints = hints(c, cookie.hints);
        w.accepts_input = observed_hints.accepts_input;
        w.initially_iconic = observed_hints.initially_iconic;
        w.urgent = observed_hints.urgent;
        w.size_hints = size_hints(c, cookie.normal_hints);
        auto supported = protocols(c, cookie.protocols);
        w.supports_take_focus = std::ranges::contains(supported, atoms_.wm_take_focus);
        w.user_time_window = window_value(c, cookie.time_window);
        w.user_time = scalar(c, cookie.time, XCB_ATOM_CARDINAL).value_or(0);
        xcb_ewmh_get_wm_fullscreen_monitors_reply_t monitors;
        if (xcb_ewmh_get_wm_fullscreen_monitors_reply(e, cookie.fullscreen_monitors, &monitors, nullptr))
            w.fullscreen_monitors = FullscreenMonitors{ monitors.top, monitors.bottom, monitors.left, monitors.right };
        auto counter_reply = xproperty::receive(c, cookie.sync_counter);
        auto counters = xproperty::words(counter_reply, XCB_ATOM_CARDINAL);
        // Basic and extended sync properties both start with the basic counter.
        if (std::ranges::contains(supported, e->_NET_WM_SYNC_REQUEST) && !counters.empty())
            observed.sync_counter = counters.front();

        // Rare dependent reads: a separate user-time window, a parent LWM does
        // not manage, and the current value of a sync counter.
        if (w.user_time_window != XCB_NONE && w.user_time_window != w.id)
        {
            watch_user_time_window(w.user_time_window);
            w.user_time = scalar(c, request(c, w.user_time_window, e->_NET_WM_USER_TIME, XCB_ATOM_CARDINAL, 1), XCB_ATOM_CARDINAL).value_or(0);
        }
        if (w.transient_for != XCB_NONE && !state_.find(w.transient_for))
            w.unmanaged_parent = read_window_geometry(w.transient_for);
        if (observed.sync_counter)
            if (auto* counter = xcb_sync_query_counter_reply(c, xcb_sync_query_counter(c, observed.sync_counter), nullptr))
            {
                observed.sync_value = (static_cast<uint64_t>(counter->counter_value.hi) << 32) | counter->counter_value.lo;
                free(counter);
            }
    }
    return result;
}

// First recognized _NET_WM_WINDOW_TYPE, or Normal.
WindowType WindowManager::window_type(xcb_get_property_cookie_t cookie) const
{
    xcb_ewmh_get_atoms_reply_t types;
    if (!xcb_ewmh_get_wm_window_type_reply(ewmh_.get(), cookie, &types, nullptr))
        return WindowType::Normal;
    auto result = ewmh_.window_type({ types.atoms, types.atoms_len });
    xcb_ewmh_get_atoms_reply_wipe(&types);
    return result;
}

// Property updates read only the changed property, with the admission decoders.
void WindowManager::handle_property_notify(xcb_property_notify_event_t const& event)
{
    auto* c = conn_.get();
    auto* e = ewmh_.get();
    xcb_window_t window = event.window;
    xcb_atom_t atom = event.atom;
    auto user_time = [&](xcb_window_t source)
    { return scalar(c, request(c, source, e->_NET_WM_USER_TIME, XCB_ATOM_CARDINAL, 1), XCB_ATOM_CARDINAL).value_or(0); };
    // User time may live on a separate, unmanaged window.
    if (atom == e->_NET_WM_USER_TIME)
        return state_.user_time(window, user_time(window));
    if (state_.find_fixture(window))
    {
        if (atom == e->_NET_WM_STRUT || atom == e->_NET_WM_STRUT_PARTIAL)
            state_.reserve(window,
                           strut(c,
                                 { request(c, window, e->_NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 12),
                                   request(c, window, e->_NET_WM_STRUT, XCB_ATOM_CARDINAL, 4) }));
        return;
    }
    auto const* client = state_.find(window);
    if (!client)
        return;
    if (atom == e->_NET_WM_NAME || atom == XCB_ATOM_WM_NAME)
    {
        // Title updates are frequent; the legacy name is read only when needed.
        auto net = text(c, request(c, window, e->_NET_WM_NAME, e->UTF8_STRING, kTitleLimit), e->UTF8_STRING);
        state_.title(window, net ? *net : text(c, request(c, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, kTitleLimit), XCB_ATOM_STRING).value_or("Unnamed"));
    }
    else if (atom == XCB_ATOM_WM_CLASS)
    {
        auto [instance, name] = wm_class(c, xcb_icccm_get_wm_class(c, window));
        state_.window_class(window, std::move(instance), std::move(name));
    }
    else if (atom == e->_NET_WM_WINDOW_TYPE)
        state_.window_type(window, window_type(xcb_ewmh_get_wm_window_type(e, window)));
    else if (atom == XCB_ATOM_WM_TRANSIENT_FOR)
        state_.transient(window, window_value(c, request(c, window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 1)));
    else if (atom == XCB_ATOM_WM_NORMAL_HINTS)
    {
        auto parent = client->transient_for;
        state_.size_hints(window,
                          size_hints(c, xcb_icccm_get_wm_normal_hints(c, window)),
                          parent && !state_.find(parent) ? read_window_geometry(parent) : std::nullopt);
    }
    else if (atom == XCB_ATOM_WM_HINTS)
    {
        auto observed = hints(c, xcb_icccm_get_wm_hints(c, window));
        state_.focus_hints(window, observed.accepts_input, client->supports_take_focus);
        // WM_HINTS is shared with the application: only a value that differs from
        // our mirror is its request, so our own write's echo changes nothing.
        // Clearing the hint withdraws only the application's urgency, and the
        // forgotten mirror lets publication reassert what State still holds.
        if (auto& output = outputs_[window]; output.urgent != observed.urgent)
        {
            state_.hint_urgency(window, observed.urgent);
            output.urgent.reset();
            presentation_dirty_ = true;
        }
    }
    else if (atom == e->WM_PROTOCOLS)
        state_.focus_hints(window, client->accepts_input, std::ranges::contains(read_protocols(window), atoms_.wm_take_focus));
    else if (atom == e->_NET_WM_USER_TIME_WINDOW)
    {
        auto time_window = window_value(c, request(c, window, e->_NET_WM_USER_TIME_WINDOW, XCB_ATOM_WINDOW, 1));
        if (time_window != XCB_NONE && time_window != window)
            watch_user_time_window(time_window);
        state_.user_time_window(window, time_window, user_time(time_window != XCB_NONE ? time_window : window));
    }
}

std::vector<xcb_atom_t> WindowManager::read_protocols(xcb_window_t window) const
{
    return protocols(conn_.get(), xcb_icccm_get_wm_protocols(conn_.get(), window, ewmh_.get()->WM_PROTOCOLS));
}

// Observe user-time updates on a separate window without replacing its mask.
void WindowManager::watch_user_time_window(xcb_window_t window)
{
    auto* attributes = xcb_get_window_attributes_reply(conn_.get(), xcb_get_window_attributes(conn_.get(), window), nullptr);
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | (attributes ? attributes->your_event_mask : 0);
    free(attributes);
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, &mask);
}

std::optional<Geometry> WindowManager::read_window_geometry(xcb_window_t window) const
{
    auto* geometry = xcb_get_geometry_reply(conn_.get(), xcb_get_geometry(conn_.get(), window), nullptr);
    if (!geometry)
        return std::nullopt;
    Geometry result{ geometry->x, geometry->y, geometry->width, geometry->height };
    free(geometry);
    return result;
}

} // namespace lwm
