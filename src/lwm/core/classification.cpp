#include "classification.hpp"

namespace lwm {

WindowClassification classify_window_type(WindowType type, bool is_transient)
{
    using enum WindowClassification::Kind;
    switch (type)
    {
        case WindowType::Desktop:
            return { Desktop, true, true };
        case WindowType::Dock:
            return { Dock, true, true };
        case WindowType::Toolbar:
        case WindowType::Menu:
        case WindowType::Splash:
            return { Floating, true, true };
        case WindowType::Utility:
            return { Floating, true, true, true };
        case WindowType::Dialog:
            return { Floating };
        case WindowType::DropdownMenu:
        case WindowType::PopupMenu:
        case WindowType::Tooltip:
        case WindowType::Notification:
        case WindowType::Combo:
        case WindowType::Dnd:
            return { Popup, true, true };
        case WindowType::Normal:
            break;
    }
    if (is_transient)
        return { Floating, true, true };
    return { Tiled };
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
    return client.preferences.skip_taskbar.value_or(defaults(client).skip_taskbar || client.transient_for != XCB_NONE);
}

bool skips_pager(Client const& client)
{
    return client.preferences.skip_pager.value_or(defaults(client).skip_pager || client.transient_for != XCB_NONE);
}

std::optional<bool> default_floating(Client const& client)
{
    auto kind = defaults(client).kind;
    if (kind != WindowClassification::Kind::Tiled && kind != WindowClassification::Kind::Floating)
        return std::nullopt;
    return kind == WindowClassification::Kind::Floating;
}

} // namespace lwm
