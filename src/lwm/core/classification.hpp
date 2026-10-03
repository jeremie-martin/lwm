#pragma once

#include "lwm/core/types.hpp"

namespace lwm {

/// Defaults derived from the first recognized `_NET_WM_WINDOW_TYPE` and transient status.
struct WindowClassification
{
    WindowRole role = WindowRole::Client;
    bool floating = false; ///< Default mode of a client
    bool skip_taskbar = false;
    bool skip_pager = false;
    bool above = false; // For UTILITY windows
};

WindowClassification classify_window_type(WindowType type, bool is_transient);

// Effective values: explicit preferences override classification defaults, and
// modal/fullscreen state project the layer without erasing the preference.
LayerHint effective_layer(Client const& client);
bool skips_taskbar(Client const& client);
bool skips_pager(Client const& client);
// The _NET_WM_STATE values LWM publishes for a client.
WindowStates published_states(Client const& client, bool focused);
/// Default mode for a normal client; nullopt when the type is not a normal client type.
std::optional<bool> default_floating(Client const& client);

} // namespace lwm
