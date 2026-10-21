// Admission adapters: observe windows, let State decide their roles, then
// install the X resources each role needs.

#include "lwm/core/xproperty.hpp"
#include "wm.hpp"
#include <algorithm>

namespace lwm {

// Hidden clients are published like visible ones, at their normal presentation.
void WindowManager::restore_clients()
{
    state_.end_drag(false);
    release_pointer();
    auto clients = state_.project(state_.fullscreen_visibility());
    for (auto& [client, presentation] : clients)
        if (!presentation)
            presentation = state_.presentation(*client);
    publish_clients(clients);
}

void WindowManager::scan_existing_windows(bool handoff)
{
    std::vector<xcb_window_t> children;
    if (auto tree = reply(xcb_query_tree_reply(conn_.get(), xcb_query_tree(conn_.get(), conn_.screen()->root), nullptr)))
    {
        auto* first = xcb_query_tree_children(tree.get());
        children.assign(first, first + xcb_query_tree_children_length(tree.get()));
    }
    // A cooperative predecessor restored mapping before release, so an unmapped
    // client has since withdrawn. Only cold recovery needs managed WM_STATE.
    auto windows = observe(children, true, !handoff);

    std::optional<std::pair<int16_t, int16_t>> pointer;
    if (!handoff)
        if (auto query = reply(xcb_query_pointer_reply(conn_.get(), xcb_query_pointer(conn_.get(), conn_.screen()->root), nullptr)))
            pointer = { query->root_x, query->root_y };
    state_.adopt(windows, handoff_ ? &*handoff_ : nullptr, pointer);
    handoff_.reset();
    for (auto const& window : windows) manage(window);
}

// X resources follow the role State chose; observation already selected its
// events. Mapping is observed, not inferred from the admission path.
void WindowManager::manage(WindowObservation const& observed)
{
    auto window = observed.id;
    if (auto const* fixture = state_.find_fixture(window))
    {
        if (fixture->role == Fixture::Role::Desktop)
        {
            // Desktops start below every sibling, not just below managed clients.
            uint32_t below = XCB_STACK_MODE_BELOW;
            xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &below);
        }
        outputs_[window].mapped = observed.mapped;
        return;
    }
    if (!state_.find(window))
    {
        if (!observed.mapped)
            xcb_map_window(conn_.get(), window);
        return;
    }
    // Seed publication with what the window already carries.
    auto& output = outputs_[window];
    output.mapped = observed.mapped;
    output.urgent = observed.urgent;
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
