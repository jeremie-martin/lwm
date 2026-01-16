#include "keybind.hpp"
#include <X11/Xlib.h>
#include <sstream>

namespace lwm {

KeybindManager::KeybindManager(Connection& conn, Config const& config)
    : conn_(conn)
{
    for (auto const& kb : config.keybinds)
    {
        uint16_t mod = parse_modifier(kb.mod);
        xcb_keysym_t keysym = parse_keysym(kb.key);

        if (keysym != XCB_NO_SYMBOL)
        {
            KeyBinding binding{ mod, keysym };
            Action action{ kb.action, kb.command, kb.workspace };
            bindings_[binding] = action;
        }
    }
}

void KeybindManager::grab_keys(xcb_window_t window)
{
    xcb_ungrab_key(conn_.get(), XCB_GRAB_ANY, window, XCB_MOD_MASK_ANY);

    for (auto const& [binding, action] : bindings_)
    {
        xcb_keycode_t* keycode = xcb_key_symbols_get_keycode(conn_.keysyms(), binding.keysym);
        if (keycode)
        {
            // Grab the key with and without Num Lock / Caps Lock
            uint16_t const modifiers[] = {
                binding.modifier,
                static_cast<uint16_t>(binding.modifier | XCB_MOD_MASK_2),
                static_cast<uint16_t>(binding.modifier | XCB_MOD_MASK_LOCK),
                static_cast<uint16_t>(binding.modifier | XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK)
            };

            for (auto mod : modifiers)
            {
                xcb_grab_key(conn_.get(), 1, window, mod, *keycode, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
            }
            free(keycode);
        }
    }

    conn_.flush();
}

std::optional<Action> KeybindManager::resolve(uint16_t state, xcb_keysym_t keysym) const
{
    uint16_t cleanMod = state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2);

    auto it = bindings_.find({ cleanMod, keysym });
    if (it != bindings_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::string KeybindManager::resolve_command(std::string const& command, Config const& config) const
{
    if (command == "terminal")
        return config.programs.terminal;
    if (command == "browser")
        return config.programs.browser;
    if (command == "launcher")
        return config.programs.launcher;
    return command;
}

uint16_t KeybindManager::parse_modifier(std::string const& mod)
{
    uint16_t result = 0;
    std::istringstream stream(mod);
    std::string token;

    while (std::getline(stream, token, '+'))
    {
        if (token == "super")
            result |= XCB_MOD_MASK_4;
        else if (token == "shift")
            result |= XCB_MOD_MASK_SHIFT;
        else if (token == "ctrl" || token == "control")
            result |= XCB_MOD_MASK_CONTROL;
        else if (token == "alt")
            result |= XCB_MOD_MASK_1;
    }

    return result;
}

xcb_keysym_t KeybindManager::parse_keysym(std::string const& key)
{
    KeySym sym = XStringToKeysym(key.c_str());
    if (sym != NoSymbol)
    {
        return static_cast<xcb_keysym_t>(sym);
    }
    return XCB_NO_SYMBOL;
}

} // namespace lwm
