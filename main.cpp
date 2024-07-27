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
        , currentTagIndex_(0)
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
        createStatusBar();
        initializeTags();

        xcb_flush(conn_.get());
    }

    void run()
    {
        while (auto event = xcb_wait_for_event(conn_.get()))
        {
            std::unique_ptr<xcb_generic_event_t, decltype(&free)> eventPtr(event, free);
            handleEvent(*eventPtr);
        }
    }

private:
    struct Window
    {
        xcb_window_t id;
        std::string name;
    };

    struct Tag
    {
        std::vector<Window> windows;
        xcb_window_t focusedWindow = XCB_NONE;
    };

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
    std::vector<Tag> tags_;
    size_t currentTagIndex_;
    xcb_window_t statusBarWindow_;

    static constexpr int NUM_TAGS = 10;
    static constexpr uint32_t PADDING = 10;
    static constexpr uint32_t FOCUS_BORDER_WIDTH = 2;
    static constexpr uint32_t FOCUS_BORDER_COLOR = 0xFF0000; // Red color
    static constexpr uint32_t STATUS_BAR_HEIGHT = 30;
    static constexpr uint32_t STATUS_BAR_COLOR = 0x808080; // Gray color
    void setupRoot()
    {
        uint32_t values[] = { XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                              | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW
                              | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE };
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

        // Add key bindings for switching tags (AZERTY layout)
        addKeyBinding(XCB_MOD_MASK_4, XK_ampersand, "switch_tag", "switch_tag_0");
        addKeyBinding(XCB_MOD_MASK_4, XK_eacute, "switch_tag", "switch_tag_1");
        addKeyBinding(XCB_MOD_MASK_4, XK_quotedbl, "switch_tag", "switch_tag_2");
        addKeyBinding(XCB_MOD_MASK_4, XK_apostrophe, "switch_tag", "switch_tag_3");
        addKeyBinding(XCB_MOD_MASK_4, XK_parenleft, "switch_tag", "switch_tag_4");
        addKeyBinding(XCB_MOD_MASK_4, XK_minus, "switch_tag", "switch_tag_5");
        addKeyBinding(XCB_MOD_MASK_4, XK_egrave, "switch_tag", "switch_tag_6");
        addKeyBinding(XCB_MOD_MASK_4, XK_underscore, "switch_tag", "switch_tag_7");
        addKeyBinding(XCB_MOD_MASK_4, XK_ccedilla, "switch_tag", "switch_tag_8");
        addKeyBinding(XCB_MOD_MASK_4, XK_agrave, "switch_tag", "switch_tag_9");

        // Add key binding for moving window to a different tag
        addKeyBinding(XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, XK_m, "move_to_tag", "move_to_tag");

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

    void initializeTags() { tags_.resize(NUM_TAGS); }

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
        focusWindow(e.window);
    }

    void handleKeyPress(xcb_key_press_event_t const& e)
    {
        xcb_keysym_t keysym = xcb_key_press_lookup_keysym(keysyms_.get(), const_cast<xcb_key_press_event_t*>(&e), 0);
        std::cout << "Received key press: modifier=" << e.state << ", keysym=" << keysym << std::endl;

        auto it = keyBindings_.find({ static_cast<uint16_t>(e.state), keysym });
        if (it != keyBindings_.end())
        {
            if (it->second.name == "kill" && tags_[currentTagIndex_].focusedWindow != XCB_NONE)
            {
                std::cout << "Killing window: " << tags_[currentTagIndex_].focusedWindow << std::endl;
                killWindow(tags_[currentTagIndex_].focusedWindow);
            }
            else if (it->second.name.substr(0, 10) == "switch_tag")
            {
                int tag = std::stoi(it->second.name.substr(11));
                switchToTag(tag);
            }
            else if (it->second.name == "move_to_tag")
            {
                moveWindowToNextTag();
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

        unmanageWindow(window);
        xcb_flush(conn_.get());
    }

    void handleUnmapNotify(xcb_unmap_notify_event_t const& e) { unmanageWindow(e.window); }

    void handleDestroyNotify(xcb_destroy_notify_event_t const& e) { unmanageWindow(e.window); }

    void handleEnterNotify(xcb_enter_notify_event_t const& e)
    {
        if (e.event != screen_->root && e.mode == XCB_NOTIFY_MODE_NORMAL)
        {
            focusWindow(e.event);
        }
    }

    void manageWindow(xcb_window_t window)
    {
        Window newWindow = { window, getWindowName(window) };
        tags_[currentTagIndex_].windows.push_back(newWindow);
        uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
                              | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
        xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
        grabKeys(window);
        rearrangeWindows();
        updateStatusBar();
    }

    void grabKeys(xcb_window_t window)
    {
        // Ungrab all keys first
        xcb_ungrab_key(conn_.get(), XCB_GRAB_ANY, window, XCB_MOD_MASK_ANY);

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
                free(keycode);
            }
        }
    }

    void unmanageWindow(xcb_window_t window)
    {
        auto& currentTag = tags_[currentTagIndex_];
        auto it = std::find_if(
            currentTag.windows.begin(),
            currentTag.windows.end(),
            [window](Window const& w) { return w.id == window; }
        );
        if (it != currentTag.windows.end())
        {
            currentTag.windows.erase(it);
            if (currentTag.focusedWindow == window)
            {
                currentTag.focusedWindow = XCB_NONE;
                if (!currentTag.windows.empty())
                {
                    focusWindow(currentTag.windows.back().id);
                }
            }
            rearrangeWindows();
            updateStatusBar();
        }
    }

    void rearrangeWindows()
    {
        std::cout << "Rearranging windows. Current tag: " << currentTagIndex_ << std::endl;

        for (auto tag : tags_)
        {
            for (auto window : tag.windows)
            {
                std::cout << "Window: " << window.id << " " << window.name << std::endl;
            }
        }

        auto& currentTag = tags_[currentTagIndex_];

        for (auto const& window : currentTag.windows)
        {
            xcb_map_window(conn_.get(), window.id);
        }

        if (currentTag.windows.empty())
            return;

        uint32_t screenWidth = screen_->width_in_pixels;
        uint32_t screenHeight = screen_->height_in_pixels - STATUS_BAR_HEIGHT;

        if (currentTag.windows.size() == 1)
        {
            configureWindow(
                currentTag.windows[0].id,
                PADDING,
                PADDING + STATUS_BAR_HEIGHT,
                screenWidth - 2 * PADDING,
                screenHeight - 2 * PADDING
            );
        }
        else if (currentTag.windows.size() == 2)
        {
            configureWindow(
                currentTag.windows[0].id,
                PADDING,
                PADDING + STATUS_BAR_HEIGHT,
                (screenWidth - 3 * PADDING) / 2,
                screenHeight - 2 * PADDING
            );
            configureWindow(
                currentTag.windows[1].id,
                (screenWidth + PADDING) / 2,
                PADDING + STATUS_BAR_HEIGHT,
                (screenWidth - 3 * PADDING) / 2,
                screenHeight - 2 * PADDING
            );
        }
        else
        {
            configureWindow(
                currentTag.windows[0].id,
                PADDING,
                PADDING + STATUS_BAR_HEIGHT,
                (screenWidth - 3 * PADDING) / 2,
                screenHeight - 2 * PADDING
            );

            uint32_t rightWidth = (screenWidth - 3 * PADDING) / 2;
            uint32_t rightHeight =
                (screenHeight - (currentTag.windows.size() * PADDING)) / (currentTag.windows.size() - 1);

            for (size_t i = 1; i < currentTag.windows.size(); ++i)
            {
                configureWindow(
                    currentTag.windows[i].id,
                    (screenWidth + PADDING) / 2,
                    PADDING + STATUS_BAR_HEIGHT + (i - 1) * (rightHeight + PADDING),
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
        auto& currentTag = tags_[currentTagIndex_];

        if (currentTag.focusedWindow != XCB_NONE)
        {
            xcb_change_window_attributes(
                conn_.get(),
                currentTag.focusedWindow,
                XCB_CW_BORDER_PIXEL,
                &screen_->black_pixel
            );
        }

        currentTag.focusedWindow = window;
        xcb_change_window_attributes(conn_.get(), currentTag.focusedWindow, XCB_CW_BORDER_PIXEL, &FOCUS_BORDER_COLOR);
        xcb_configure_window(
            conn_.get(),
            currentTag.focusedWindow,
            XCB_CONFIG_WINDOW_BORDER_WIDTH,
            &FOCUS_BORDER_WIDTH
        );
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, currentTag.focusedWindow, XCB_CURRENT_TIME);

        // Re-grab keys for the newly focused window
        grabKeys(currentTag.focusedWindow);

        updateStatusBar();
        xcb_flush(conn_.get());
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

    void switchToTag(int tag)
    {
        if (tag < 0 || tag >= NUM_TAGS || tag == static_cast<int>(currentTagIndex_))
            return;

        // Unmap all windows on the current tag
        for (auto const& window : tags_[currentTagIndex_].windows)
        {
            xcb_unmap_window(conn_.get(), window.id);
        }

        currentTagIndex_ = tag;
        rearrangeWindows();

        // Restore focus on the new tag
        if (tags_[currentTagIndex_].focusedWindow != XCB_NONE)
        {
            focusWindow(tags_[currentTagIndex_].focusedWindow);
        }
        else if (!tags_[currentTagIndex_].windows.empty())
        {
            focusWindow(tags_[currentTagIndex_].windows.back().id);
        }

        updateStatusBar();
    }

    void moveWindowToNextTag()
    {
        if (tags_[currentTagIndex_].focusedWindow == XCB_NONE)
            return;

        size_t nextTagIndex = (currentTagIndex_ + 1) % NUM_TAGS;
        xcb_window_t windowToMove = tags_[currentTagIndex_].focusedWindow;

        // Find and remove the window from the current tag
        auto& currentTag = tags_[currentTagIndex_];
        auto it = std::find_if(
            currentTag.windows.begin(),
            currentTag.windows.end(),
            [windowToMove](Window const& w) { return w.id == windowToMove; }
        );
        if (it != currentTag.windows.end())
        {
            Window movedWindow = *it;
            currentTag.windows.erase(it);
            currentTag.focusedWindow = XCB_NONE;

            // Add the window to the next tag
            tags_[nextTagIndex].windows.push_back(movedWindow);
            tags_[nextTagIndex].focusedWindow = windowToMove;

            // Unmap the window as it's no longer on the current tag
            xcb_unmap_window(conn_.get(), windowToMove);

            rearrangeWindows();
            updateStatusBar();

            // Focus the next window on the current tag, if any
            if (!currentTag.windows.empty())
            {
                focusWindow(currentTag.windows.back().id);
            }
        }
    }

    void createStatusBar()
    {
        uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        uint32_t values[2] = { STATUS_BAR_COLOR, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS };

        statusBarWindow_ = xcb_generate_id(conn_.get());
        xcb_create_window(
            conn_.get(),
            XCB_COPY_FROM_PARENT,
            statusBarWindow_,
            screen_->root,
            0,
            0,
            screen_->width_in_pixels,
            STATUS_BAR_HEIGHT,
            0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            screen_->root_visual,
            mask,
            values
        );

        xcb_map_window(conn_.get(), statusBarWindow_);
    }

    void updateStatusBar()
    {
        std::string statusText = "Tags: ";
        for (size_t i = 0; i < NUM_TAGS; ++i)
        {
            statusText += (i == currentTagIndex_ ? "[" : " ") + std::to_string(i) + (i == currentTagIndex_ ? "]" : " ");
        }

        statusText += " | Focused: ";
        if (tags_[currentTagIndex_].focusedWindow != XCB_NONE)
        {
            auto it = std::find_if(
                tags_[currentTagIndex_].windows.begin(),
                tags_[currentTagIndex_].windows.end(),
                [this](Window const& w) { return w.id == tags_[currentTagIndex_].focusedWindow; }
            );
            if (it != tags_[currentTagIndex_].windows.end())
            {
                statusText += it->name;
            }
            else
            {
                statusText += "Unknown";
            }
        }
        else
        {
            statusText += "None";
        }

        xcb_clear_area(conn_.get(), 0, statusBarWindow_, 0, 0, 0, 0);

        xcb_font_t font = xcb_generate_id(conn_.get());
        xcb_open_font(conn_.get(), font, strlen("fixed"), "fixed");

        xcb_gcontext_t gc = xcb_generate_id(conn_.get());
        uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT;
        uint32_t values[3] = { screen_->black_pixel, STATUS_BAR_COLOR, font };
        xcb_create_gc(conn_.get(), gc, statusBarWindow_, mask, values);

        xcb_image_text_8(
            conn_.get(),
            statusText.length(),
            statusBarWindow_,
            gc,
            10,
            STATUS_BAR_HEIGHT - 5,
            statusText.c_str()
        );

        xcb_close_font(conn_.get(), font);
        xcb_free_gc(conn_.get(), gc);

        xcb_flush(conn_.get());
    }

    std::string getWindowName(xcb_window_t window)
    {
        xcb_get_property_cookie_t cookie =
            xcb_get_property(conn_.get(), 0, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 1024);
        xcb_get_property_reply_t* reply = xcb_get_property_reply(conn_.get(), cookie, NULL);

        if (reply)
        {
            int len = xcb_get_property_value_length(reply);
            char* name = static_cast<char*>(xcb_get_property_value(reply));
            std::string windowName(name, len);
            free(reply);
            return windowName;
        }
        return "Unnamed";
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