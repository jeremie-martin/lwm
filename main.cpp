#include <X11/keysym.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xproto.h>

class WindowManager
{
public:
    WindowManager()
        : conn_(xcb_connect(nullptr, nullptr), xcb_disconnect)
        , screen_(nullptr)
        , keysyms_(nullptr, xcb_key_symbols_free)
    {
        if (xcb_connection_has_error(conn_.get()))
        {
            throw std::runtime_error("Failed to connect to X server");
        }

        screen_ = xcb_setup_roots_iterator(xcb_get_setup(conn_.get())).data;
        if (!screen_)
        {
            throw std::runtime_error("Failed to get screen");
        }

        setupRoot();
        setupKeySymbols();
        setupKeyBindings();

        xcb_flush(conn_.get());
    }

    void run()
    {
        while (auto event = xcb_wait_for_event(conn_.get()))
        {
            std::unique_ptr<xcb_generic_event_t, decltype(&free)> eventPtr(event, free);
            handleEvent(*eventPtr);
            processFocusQueue();
        }
    }

private:
    struct Program
    {
        std::string path;
        std::string name;
    };

    struct KeyBinding
    {
        uint16_t modifier;
        xcb_keysym_t keysym;

        bool operator<(KeyBinding const& other) const
        {
            return std::tie(modifier, keysym) < std::tie(other.modifier, other.keysym);
        }
    };

    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> conn_;
    xcb_screen_t* screen_;
    std::unique_ptr<xcb_key_symbols_t, decltype(&xcb_key_symbols_free)> keysyms_;

    std::map<KeyBinding, Program> keyBindings_;
    std::vector<xcb_window_t> managedWindows_;
    xcb_window_t focusedWindow_ = XCB_NONE;
    std::queue<xcb_window_t> focusQueue_;

    static constexpr uint32_t PADDING = 10;
    static constexpr uint32_t FOCUS_BORDER_WIDTH = 2;
    static constexpr uint32_t FOCUS_BORDER_COLOR = 0xFF0000; // Red color

    void setupRoot()
    {
        uint32_t values[] = { XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                              | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW
                              | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE
                              | XCB_EVENT_MASK_KEY_PRESS };
        xcb_change_window_attributes(conn_.get(), screen_->root, XCB_CW_EVENT_MASK, values);
    }

    void setupKeySymbols()
    {
        keysyms_.reset(xcb_key_symbols_alloc(conn_.get()));
        if (!keysyms_)
        {
            throw std::runtime_error("Failed to allocate key symbols");
        }
    }

    void setupKeyBindings()
    {
        addKeyBinding(XCB_MOD_MASK_4, XK_f, "/usr/bin/firefox", "firefox");
        addKeyBinding(XCB_MOD_MASK_4, XK_w, "/usr/bin/brave-browser-beta", "brave-browser-beta");
        addKeyBinding(XCB_MOD_MASK_4, XK_Return, "/usr/local/bin/st", "st");
        addKeyBinding(XCB_MOD_MASK_4, XK_q, "kill", "kill");
        addKeyBinding(XCB_MOD_MASK_4, XK_d, "dmenu_run", "dmenu_run");

        grabKeys(screen_->root);
    }

    void addKeyBinding(uint16_t modifier, xcb_keysym_t keysym, std::string const& path, std::string const& name)
    {
        keyBindings_[{ modifier, keysym }] = { path, name };
        xcb_keycode_t* keycode = xcb_key_symbols_get_keycode(keysyms_.get(), keysym);
        if (keycode)
        {
            xcb_grab_key(conn_.get(), 1, screen_->root, modifier, *keycode, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        }
    }

    void handleEvent(xcb_generic_event_t const& event)
    {
        switch (event.response_type & ~0x80)
        {
            case XCB_MAP_REQUEST:
                handleMapRequest(reinterpret_cast<xcb_map_request_event_t const&>(event));
                break;
            case XCB_UNMAP_NOTIFY:
                handleUnmapNotify(reinterpret_cast<xcb_unmap_notify_event_t const&>(event));
                break;
            case XCB_DESTROY_NOTIFY:
                handleDestroyNotify(reinterpret_cast<xcb_destroy_notify_event_t const&>(event));
                break;
            case XCB_ENTER_NOTIFY:
                handleEnterNotify(reinterpret_cast<xcb_enter_notify_event_t const&>(event));
                break;
            case XCB_KEY_PRESS:
                handleKeyPress(reinterpret_cast<xcb_key_press_event_t const&>(event));
                break;
        }
    }

    void handleMapRequest(xcb_map_request_event_t const& e)
    {
        manageWindow(e.window);
        xcb_map_window(conn_.get(), e.window);
        focusQueue_.push(e.window);
    }

    void handleKeyPress(xcb_key_press_event_t const& e)
    {
        xcb_keysym_t keysym = xcb_key_press_lookup_keysym(keysyms_.get(), const_cast<xcb_key_press_event_t*>(&e), 0);
        std::cout << "Received key press: modifier=" << e.state << ", keysym=" << keysym << std::endl;

        auto it = keyBindings_.find({ static_cast<uint16_t>(e.state), keysym });
        if (it == keyBindings_.end())
        {
            it = std::find_if(
                keyBindings_.begin(),
                keyBindings_.end(),
                [keysym](auto const& pair) { return pair.first.keysym == keysym; }
            );
        }

        if (it != keyBindings_.end())
        {
            if (it->second.name == "kill" && focusedWindow_ != XCB_NONE)
            {
                std::cout << "Killing window: " << focusedWindow_ << std::endl;
                killWindow(focusedWindow_);
            }
            else
            {
                std::cout << "Launching program: " << it->second.name << std::endl;
                launchProgram(it->second);
            }
        }
        else
        {
            std::cout << "No program associated with this key binding" << std::endl;
        }
    }

    void killWindow(xcb_window_t window)
    {
        // First, try to kill the window nicely
        xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn_.get(), 0, 16, "WM_DELETE_WINDOW");
        xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn_.get(), cookie, nullptr);

        if (reply)
        {
            xcb_client_message_event_t ev = { 0 };
            ev.response_type = XCB_CLIENT_MESSAGE;
            ev.window = window;
            ev.type = reply->atom;
            ev.format = 32;
            ev.data.data32[0] = reply->atom;
            ev.data.data32[1] = XCB_CURRENT_TIME;

            xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, (char*)&ev);
            free(reply);
        }

        // Wait a short time for the window to close
        usleep(100000); // 100ms

        // Check if the window still exists
        xcb_get_window_attributes_cookie_t attr_cookie = xcb_get_window_attributes(conn_.get(), window);
        xcb_get_window_attributes_reply_t* attr_reply =
            xcb_get_window_attributes_reply(conn_.get(), attr_cookie, nullptr);

        if (attr_reply)
        {
            // Window still exists, force kill it
            free(attr_reply);
            xcb_kill_client(conn_.get(), window);
        }

        xcb_flush(conn_.get());
    }

    void handleUnmapNotify(xcb_unmap_notify_event_t const& e) { unmanageWindow(e.window); }

    void handleDestroyNotify(xcb_destroy_notify_event_t const& e) { unmanageWindow(e.window); }

    void handleEnterNotify(xcb_enter_notify_event_t const& e)
    {
        if (e.event != screen_->root && e.mode == XCB_NOTIFY_MODE_NORMAL)
        {
            focusQueue_.push(e.event);
        }
    }

    void manageWindow(xcb_window_t window)
    {
        managedWindows_.push_back(window);
        uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
                              | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
        xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
        grabKeys(window);
        rearrangeWindows();
    }

    void grabKeys(xcb_window_t window)
    {
        for (auto const& binding : keyBindings_)
        {
            xcb_keycode_t* keycode = xcb_key_symbols_get_keycode(keysyms_.get(), binding.first.keysym);
            if (keycode)
            {
                xcb_grab_key(
                    conn_.get(),
                    1,
                    window,
                    binding.first.modifier,
                    *keycode,
                    XCB_GRAB_MODE_ASYNC,
                    XCB_GRAB_MODE_ASYNC
                );
                xcb_grab_key(
                    conn_.get(),
                    1,
                    window,
                    binding.first.modifier | XCB_MOD_MASK_2,
                    *keycode,
                    XCB_GRAB_MODE_ASYNC,
                    XCB_GRAB_MODE_ASYNC
                );
            }
        }
    }

    void unmanageWindow(xcb_window_t window)
    {
        auto it = std::find(managedWindows_.begin(), managedWindows_.end(), window);
        if (it != managedWindows_.end())
        {
            managedWindows_.erase(it);
            if (focusedWindow_ == window)
            {
                focusedWindow_ = XCB_NONE;
                if (!managedWindows_.empty())
                {
                    focusQueue_.push(managedWindows_.back());
                }
            }
            rearrangeWindows();
        }
    }

    void rearrangeWindows()
    {
        std::cout << "Rearranging windows. Total windows: " << managedWindows_.size() << std::endl;
        if (managedWindows_.empty())
            return;

        uint32_t screenWidth = screen_->width_in_pixels;
        uint32_t screenHeight = screen_->height_in_pixels;

        if (managedWindows_.size() == 1)
        {
            configureWindow(
                managedWindows_[0],
                PADDING,
                PADDING,
                screenWidth - 2 * PADDING,
                screenHeight - 2 * PADDING
            );
        }
        else if (managedWindows_.size() == 2)
        {
            configureWindow(
                managedWindows_[0],
                PADDING,
                PADDING,
                (screenWidth - 3 * PADDING) / 2,
                screenHeight - 2 * PADDING
            );
            configureWindow(
                managedWindows_[1],
                (screenWidth + PADDING) / 2,
                PADDING,
                (screenWidth - 3 * PADDING) / 2,
                screenHeight - 2 * PADDING
            );
        }
        else
        {
            configureWindow(
                managedWindows_[0],
                PADDING,
                PADDING,
                (screenWidth - 3 * PADDING) / 2,
                screenHeight - 2 * PADDING
            );

            uint32_t rightWidth = (screenWidth - 3 * PADDING) / 2;
            uint32_t rightHeight = (screenHeight - (managedWindows_.size() * PADDING)) / (managedWindows_.size() - 1);

            for (size_t i = 1; i < managedWindows_.size(); ++i)
            {
                configureWindow(
                    managedWindows_[i],
                    (screenWidth + PADDING) / 2,
                    PADDING + (i - 1) * (rightHeight + PADDING),
                    rightWidth,
                    rightHeight
                );
            }
        }

        xcb_flush(conn_.get());
    }

    void configureWindow(xcb_window_t window, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
    {
        uint32_t values[] = { x, y, width, height };
        xcb_configure_window(
            conn_.get(),
            window,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            values
        );
        std::cout << "Configured window " << window << ": x=" << x << ", y=" << y << ", width=" << width
                  << ", height=" << height << std::endl;
    }

    void focusWindow(xcb_window_t window)
    {
        if (focusedWindow_ != XCB_NONE)
        {
            xcb_change_window_attributes(conn_.get(), focusedWindow_, XCB_CW_BORDER_PIXEL, &screen_->black_pixel);
        }

        focusedWindow_ = window;
        xcb_change_window_attributes(conn_.get(), focusedWindow_, XCB_CW_BORDER_PIXEL, &FOCUS_BORDER_COLOR);
        xcb_configure_window(conn_.get(), focusedWindow_, XCB_CONFIG_WINDOW_BORDER_WIDTH, &FOCUS_BORDER_WIDTH);
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, focusedWindow_, XCB_CURRENT_TIME);

        // Re-grab keys for the newly focused window
        grabKeys(focusedWindow_);

        xcb_flush(conn_.get());
    }

    void processFocusQueue()
    {
        while (!focusQueue_.empty())
        {
            xcb_window_t window = focusQueue_.front();
            focusQueue_.pop();

            // Check if the window is still valid and managed
            auto it = std::find(managedWindows_.begin(), managedWindows_.end(), window);
            if (it != managedWindows_.end())
            {
                focusWindow(window);
                break; // Process only one focus change per event loop iteration
            }
        }
    }

    void launchProgram(Program const& program)
    {
        if (fork() == 0)
        {
            setsid();
            execl(program.path.c_str(), program.name.c_str(), nullptr);
            exit(1);
        }
    }
};

int main()
{
    try
    {
        std::cout << "Starting window manager" << std::endl;
        WindowManager wm;
        wm.run();
    }
    catch (std::exception const& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    std::cout << "Window manager exiting" << std::endl;
    return 0;
}