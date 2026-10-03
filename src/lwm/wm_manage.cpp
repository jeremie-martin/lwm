// Admission adapters: observe windows, let State decide their roles, then
// install the X resources each role needs.

#include "wm.hpp"
#include <algorithm>

namespace lwm {

void WindowManager::scan_existing_windows(bool handoff)
{
    // Only viewable, redirected windows are adopted; others are never observed further.
    std::vector<xcb_window_t> candidates;
    if (auto* tree = xcb_query_tree_reply(conn_.get(), xcb_query_tree(conn_.get(), conn_.screen()->root), nullptr))
    {
        std::span children(xcb_query_tree_children(tree), static_cast<size_t>(xcb_query_tree_children_length(tree)));
        std::vector<xcb_get_window_attributes_cookie_t> cookies;
        for (auto window : children) cookies.push_back(xcb_get_window_attributes(conn_.get(), window));
        for (size_t i = 0; i < children.size(); ++i)
            if (auto* attributes = xcb_get_window_attributes_reply(conn_.get(), cookies[i], nullptr))
            {
                if (attributes->map_state == XCB_MAP_STATE_VIEWABLE && !attributes->override_redirect)
                    candidates.push_back(children[i]);
                free(attributes);
            }
        free(tree);
    }
    auto observed = observe(candidates);
    std::erase_if(observed, [](auto const& window) { return !window.exists; });
    std::vector<WindowObservation> windows;
    for (auto const& window : observed) windows.push_back(window.window);

    std::optional<std::pair<int16_t, int16_t>> pointer;
    if (!handoff)
        if (auto* reply = xcb_query_pointer_reply(conn_.get(), xcb_query_pointer(conn_.get(), conn_.screen()->root), nullptr))
        {
            pointer = { reply->root_x, reply->root_y };
            free(reply);
        }
    state_.adopt(windows, handoff_ ? &*handoff_ : nullptr, pointer);
    handoff_.reset();
    for (auto const& window : observed) manage(window, true);
    if (!handoff)
        for (auto const& command : config().autostart) launch_program(command, "autostart");
}

// X resources follow the role State chose. Adopted windows are already mapped;
// live popups are mapped directly and otherwise left alone.
void WindowManager::manage(Observed const& observed, bool adopting)
{
    auto window = observed.window.id;
    if (auto const* fixture = state_.find_fixture(window))
    {
        uint32_t mask = fixture->role == Fixture::Role::Dock ? kDockEventMask : XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, &mask);
        if (fixture->role == Fixture::Role::Desktop)
        {
            // Desktops start below every sibling, not just below managed clients.
            uint32_t below = XCB_STACK_MODE_BELOW;
            xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &below);
        }
        outputs_[window].mapped = adopting;
        return;
    }
    if (!state_.find(window))
    {
        if (!adopting)
            xcb_map_window(conn_.get(), window);
        return;
    }
    // Seed publication with what the window already carries.
    auto& output = outputs_[window];
    output.mapped = adopting;
    output.urgent = observed.window.urgent;
    output.fullscreen_monitors.emplace(observed.window.fullscreen_monitors);
    output.sync_counter = observed.sync_counter;
    output.sync_value = observed.sync_value;
    uint32_t mask = kManagedWindowEventMask;
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, &mask);
    xcb_grab_button(
        conn_.get(),
        0,
        window,
        XCB_EVENT_MASK_BUTTON_PRESS,
        XCB_GRAB_MODE_SYNC,
        XCB_GRAB_MODE_ASYNC,
        XCB_NONE,
        XCB_NONE,
        XCB_BUTTON_INDEX_ANY,
        XCB_MOD_MASK_ANY
    );
    xcb_ewmh_set_frame_extents(ewmh_.get(), window, 0, 0, 0, 0);
}

} // namespace lwm
