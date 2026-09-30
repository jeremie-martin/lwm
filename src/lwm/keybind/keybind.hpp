#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/types.hpp"
#include <optional>

namespace lwm {

class KeybindManager
{
public:
    KeybindManager(Connection& conn, Config const& config);

    void grab_keys(xcb_window_t window);
    std::optional<Action> resolve(uint16_t state, xcb_keysym_t keysym) const;

private:
    Connection& conn_;
    Config const& config_; // The owning object must outlive this manager.
};

} // namespace lwm
