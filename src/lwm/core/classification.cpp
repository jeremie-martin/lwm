#include "classification.hpp"

namespace lwm {

WindowClassification classify_window_type(WindowType type, bool is_transient)
{
    using enum WindowRole;
    switch (type)
    {
        case WindowType::Desktop:
            return { Desktop, false, true };
        case WindowType::Dock:
            return { Dock, false, true };
        case WindowType::Toolbar:
        case WindowType::Menu:
        case WindowType::Splash:
            return { Client, true, true };
        case WindowType::Utility:
            return { Client, true, true, true };
        case WindowType::Dialog:
            return { Client, true, is_transient };
        case WindowType::DropdownMenu:
        case WindowType::PopupMenu:
        case WindowType::Tooltip:
        case WindowType::Notification:
        case WindowType::Combo:
        case WindowType::Dnd:
            return { Popup, false, true };
        case WindowType::Normal:
            break;
    }
    return { Client, is_transient, is_transient };
}

namespace {
WindowClassification defaults(Client const& client)
{
    return classify_window_type(client.ewmh_type, client.transient_for != XCB_NONE);
}
}

LayerHint effective_layer(Client const& client)
{
    if (client.fullscreen)
        return LayerHint::Normal;
    if (client.modal)
        return LayerHint::Above;
    return client.preferences.layer.value_or(defaults(client).above ? LayerHint::Above : LayerHint::Normal);
}

bool skips_taskbar(Client const& client)
{
    return client.preferences.skip_taskbar.value_or(defaults(client).skip);
}

bool skips_pager(Client const& client)
{
    return client.preferences.skip_pager.value_or(defaults(client).skip);
}

WindowStates published_states(Client const& client, bool focused)
{
    auto layer = effective_layer(client);
    WindowStates states;
    states.set(WindowState::Fullscreen, client.fullscreen);
    states.set(WindowState::Above, layer == LayerHint::Above);
    states.set(WindowState::Below, layer == LayerHint::Below);
    states.set(WindowState::Sticky, client.sticky);
    states.set(WindowState::Modal, client.modal);
    states.set(WindowState::SkipTaskbar, skips_taskbar(client));
    states.set(WindowState::SkipPager, skips_pager(client));
    states.set(WindowState::MaximizedHorz, client.maximized_horz);
    states.set(WindowState::MaximizedVert, client.maximized_vert);
    states.set(WindowState::Hidden, client.iconic);
    states.set(WindowState::DemandsAttention, client.urgency.active());
    states.set(WindowState::Focused, focused);
    return states;
}

std::optional<bool> default_floating(Client const& client)
{
    auto classification = defaults(client);
    return classification.role == WindowRole::Client ? std::optional{ classification.floating } : std::nullopt;
}

} // namespace lwm
