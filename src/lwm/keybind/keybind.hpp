#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/connection.hpp"
#include "lwm/core/types.hpp"
#include <map>
#include <optional>

namespace lwm {

class KeybindManager
{
public:
    KeybindManager(Connection& conn, Config const& config);

    void grab_keys(xcb_window_t window);
    std::optional<Action> resolve(uint16_t state, xcb_keysym_t keysym) const;

    std::string resolve_command(std::string const& command, Config const& config) const;

private:
    Connection& conn_;
    std::map<KeyBinding, Action> bindings_;

    static uint16_t parse_modifier(std::string const& mod);
    static xcb_keysym_t parse_keysym(std::string const& key);
};

} // namespace lwm
