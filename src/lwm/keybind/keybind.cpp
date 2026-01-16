#include "keybind.hpp"
#include <X11/keysym.h>

namespace lwm
{

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
            Action action{ kb.action, kb.command, kb.tag };
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
    // Remove Num Lock and Caps Lock from the modifier
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

    if (mod.find("super") != std::string::npos)
        result |= XCB_MOD_MASK_4;
    if (mod.find("shift") != std::string::npos)
        result |= XCB_MOD_MASK_SHIFT;
    if (mod.find("ctrl") != std::string::npos || mod.find("control") != std::string::npos)
        result |= XCB_MOD_MASK_CONTROL;
    if (mod.find("alt") != std::string::npos)
        result |= XCB_MOD_MASK_1;

    return result;
}

xcb_keysym_t KeybindManager::parse_keysym(std::string const& key)
{
    // Common key names
    static std::map<std::string, xcb_keysym_t> const keymap = {
        { "Return", XK_Return },
        { "Enter", XK_Return },
        { "Escape", XK_Escape },
        { "Tab", XK_Tab },
        { "BackSpace", XK_BackSpace },
        { "Delete", XK_Delete },
        { "space", XK_space },
        { "Space", XK_space },
        // Letters
        { "a", XK_a },
        { "b", XK_b },
        { "c", XK_c },
        { "d", XK_d },
        { "e", XK_e },
        { "f", XK_f },
        { "g", XK_g },
        { "h", XK_h },
        { "i", XK_i },
        { "j", XK_j },
        { "k", XK_k },
        { "l", XK_l },
        { "m", XK_m },
        { "n", XK_n },
        { "o", XK_o },
        { "p", XK_p },
        { "q", XK_q },
        { "r", XK_r },
        { "s", XK_s },
        { "t", XK_t },
        { "u", XK_u },
        { "v", XK_v },
        { "w", XK_w },
        { "x", XK_x },
        { "y", XK_y },
        { "z", XK_z },
        // Numbers
        { "0", XK_0 },
        { "1", XK_1 },
        { "2", XK_2 },
        { "3", XK_3 },
        { "4", XK_4 },
        { "5", XK_5 },
        { "6", XK_6 },
        { "7", XK_7 },
        { "8", XK_8 },
        { "9", XK_9 },
        // AZERTY keys
        { "ampersand", XK_ampersand },
        { "eacute", XK_eacute },
        { "quotedbl", XK_quotedbl },
        { "apostrophe", XK_apostrophe },
        { "parenleft", XK_parenleft },
        { "minus", XK_minus },
        { "egrave", XK_egrave },
        { "underscore", XK_underscore },
        { "ccedilla", XK_ccedilla },
        { "agrave", XK_agrave },
        // Function keys
        { "F1", XK_F1 },
        { "F2", XK_F2 },
        { "F3", XK_F3 },
        { "F4", XK_F4 },
        { "F5", XK_F5 },
        { "F6", XK_F6 },
        { "F7", XK_F7 },
        { "F8", XK_F8 },
        { "F9", XK_F9 },
        { "F10", XK_F10 },
        { "F11", XK_F11 },
        { "F12", XK_F12 },
    };

    auto it = keymap.find(key);
    if (it != keymap.end())
    {
        return it->second;
    }

    return XCB_NO_SYMBOL;
}

} // namespace lwm
