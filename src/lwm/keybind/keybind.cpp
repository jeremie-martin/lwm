#include "keybind.hpp"

namespace lwm {

KeybindManager::KeybindManager(Connection& conn, Config const& config)
    : conn_(conn)
    , config_(config)
{ }

void KeybindManager::grab_keys(xcb_window_t window)
{
    xcb_ungrab_key(conn_.get(), XCB_GRAB_ANY, window, XCB_MOD_MASK_ANY);

    for (auto const& [binding, action] : config_.keybinds)
    {
        xcb_keycode_t* keycodes = xcb_key_symbols_get_keycode(conn_.keysyms(), binding.keysym);
        if (keycodes)
        {
            for (xcb_keycode_t* keycode = keycodes; *keycode != XCB_NO_SYMBOL; ++keycode)
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
            }
            free(keycodes);
        }
    }

    conn_.flush();
}

std::optional<Action> KeybindManager::resolve(uint16_t state, xcb_keysym_t keysym) const
{
    uint16_t cleanMod = state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2);

    auto it = config_.keybinds.find({ cleanMod, keysym });
    if (it != config_.keybinds.end())
    {
        return it->second;
    }
    return std::nullopt;
}

} // namespace lwm
