#include "wm.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <ranges>
#include <unistd.h>

namespace lwm
{

WindowManager::WindowManager(Config config)
    : config_(std::move(config))
    , conn_()
    , keybinds_(conn_, config_)
    , layout_(conn_, config_.appearance)
    , bar_(conn_, config_.appearance, Config::NUM_TAGS)
    , tags_(Config::NUM_TAGS)
    , current_tag_(0)
{
    setup_root();
    keybinds_.grab_keys(conn_.screen()->root);
    bar_.update(current_tag_, tags_);
    conn_.flush();
}

void WindowManager::run()
{
    while (auto event = xcb_wait_for_event(conn_.get()))
    {
        std::unique_ptr<xcb_generic_event_t, decltype(&free)> eventPtr(event, free);
        handle_event(*eventPtr);
    }
}

void WindowManager::setup_root()
{
    uint32_t values[] = { XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                          | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_STRUCTURE_NOTIFY
                          | XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(conn_.get(), conn_.screen()->root, XCB_CW_EVENT_MASK, values);
}

void WindowManager::handle_event(xcb_generic_event_t const& event)
{
    switch (event.response_type & ~0x80)
    {
        case XCB_MAP_REQUEST:
            handle_map_request(reinterpret_cast<xcb_map_request_event_t const&>(event));
            break;
        case XCB_UNMAP_NOTIFY:
            handle_unmap_notify(reinterpret_cast<xcb_unmap_notify_event_t const&>(event));
            break;
        case XCB_DESTROY_NOTIFY:
            handle_destroy_notify(reinterpret_cast<xcb_destroy_notify_event_t const&>(event));
            break;
        case XCB_ENTER_NOTIFY:
            handle_enter_notify(reinterpret_cast<xcb_enter_notify_event_t const&>(event));
            break;
        case XCB_KEY_PRESS:
            handle_key_press(reinterpret_cast<xcb_key_press_event_t const&>(event));
            break;
    }
}

void WindowManager::handle_map_request(xcb_map_request_event_t const& e)
{
    manage_window(e.window);
    xcb_map_window(conn_.get(), e.window);
    focus_window(e.window);
}

void WindowManager::handle_unmap_notify(xcb_unmap_notify_event_t const& e) { unmanage_window(e.window); }

void WindowManager::handle_destroy_notify(xcb_destroy_notify_event_t const& e) { unmanage_window(e.window); }

void WindowManager::handle_enter_notify(xcb_enter_notify_event_t const& e)
{
    if (e.event != conn_.screen()->root && e.mode == XCB_NOTIFY_MODE_NORMAL)
    {
        focus_window(e.event);
    }
}

void WindowManager::handle_key_press(xcb_key_press_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_press_event_t*>(&e), 0);
    std::cout << "Received key press: modifier=" << e.state << ", keysym=" << keysym << std::endl;

    auto action = keybinds_.resolve(e.state, keysym);
    if (action)
    {
        std::cout << "Executing action: " << action->type << std::endl;

        if (action->type == "kill" && tags_[current_tag_].focusedWindow != XCB_NONE)
        {
            kill_window(tags_[current_tag_].focusedWindow);
        }
        else if (action->type == "switch_tag" && action->tag >= 0)
        {
            switch_to_tag(action->tag);
        }
        else if (action->type == "move_to_tag")
        {
            move_window_to_next_tag();
        }
        else if (action->type == "spawn")
        {
            std::string path = keybinds_.resolve_command(action->command, config_);
            launch_program(path);
        }
    }
    else
    {
        std::cout << "No action associated with this key binding" << std::endl;
    }
}

void WindowManager::manage_window(xcb_window_t window)
{
    Window newWindow = { window, get_window_name(window) };
    tags_[current_tag_].windows.push_back(newWindow);

    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
                          | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);

    keybinds_.grab_keys(window);
    rearrange_windows();
    bar_.update(current_tag_, tags_);
}

void WindowManager::unmanage_window(xcb_window_t window)
{
    auto& currentTag = tags_[current_tag_];
    auto it = std::ranges::find_if(currentTag.windows, [window](Window const& w) { return w.id == window; });
    if (it != currentTag.windows.end())
    {
        currentTag.windows.erase(it);
        if (currentTag.focusedWindow == window)
        {
            currentTag.focusedWindow = XCB_NONE;
            if (!currentTag.windows.empty())
            {
                focus_window(currentTag.windows.back().id);
            }
        }
        rearrange_windows();
        bar_.update(current_tag_, tags_);
    }
}

void WindowManager::focus_window(xcb_window_t window)
{
    auto& currentTag = tags_[current_tag_];

    if (currentTag.focusedWindow != XCB_NONE)
    {
        xcb_change_window_attributes(conn_.get(), currentTag.focusedWindow, XCB_CW_BORDER_PIXEL, &conn_.screen()->black_pixel);
    }

    currentTag.focusedWindow = window;
    xcb_change_window_attributes(conn_.get(), currentTag.focusedWindow, XCB_CW_BORDER_PIXEL, &config_.appearance.border_color);
    xcb_configure_window(conn_.get(), currentTag.focusedWindow, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);
    xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, currentTag.focusedWindow, XCB_CURRENT_TIME);

    bar_.update(current_tag_, tags_);
    conn_.flush();
}

void WindowManager::kill_window(xcb_window_t window)
{
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

        xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
        free(reply);
    }

    usleep(100000); // 100ms

    xcb_get_window_attributes_cookie_t attr_cookie = xcb_get_window_attributes(conn_.get(), window);
    xcb_get_window_attributes_reply_t* attr_reply = xcb_get_window_attributes_reply(conn_.get(), attr_cookie, nullptr);

    if (attr_reply)
    {
        free(attr_reply);
        xcb_kill_client(conn_.get(), window);
    }

    unmanage_window(window);
    conn_.flush();
}

void WindowManager::rearrange_windows()
{
    std::cout << "Rearranging windows. Current tag: " << current_tag_ << std::endl;

    auto& currentTag = tags_[current_tag_];

    // Unmap windows on other tags, map windows on current tag
    for (size_t i = 0; i < tags_.size(); ++i)
    {
        if (i != current_tag_)
        {
            for (auto const& window : tags_[i].windows)
            {
                xcb_unmap_window(conn_.get(), window.id);
            }
        }
    }

    layout_.arrange(currentTag.windows);
}

void WindowManager::switch_to_tag(int tag)
{
    if (tag < 0 || tag >= Config::NUM_TAGS || tag == static_cast<int>(current_tag_))
        return;

    for (auto const& window : tags_[current_tag_].windows)
    {
        xcb_unmap_window(conn_.get(), window.id);
    }

    current_tag_ = tag;
    rearrange_windows();

    if (tags_[current_tag_].focusedWindow != XCB_NONE)
    {
        focus_window(tags_[current_tag_].focusedWindow);
    }
    else if (!tags_[current_tag_].windows.empty())
    {
        focus_window(tags_[current_tag_].windows.back().id);
    }

    bar_.update(current_tag_, tags_);
}

void WindowManager::move_window_to_next_tag()
{
    if (tags_[current_tag_].focusedWindow == XCB_NONE)
        return;

    size_t nextTagIndex = (current_tag_ + 1) % Config::NUM_TAGS;
    xcb_window_t windowToMove = tags_[current_tag_].focusedWindow;

    auto& currentTag = tags_[current_tag_];
    auto it = std::ranges::find_if(currentTag.windows, [windowToMove](Window const& w) { return w.id == windowToMove; });
    if (it != currentTag.windows.end())
    {
        Window movedWindow = *it;
        currentTag.windows.erase(it);
        currentTag.focusedWindow = XCB_NONE;

        tags_[nextTagIndex].windows.push_back(movedWindow);
        tags_[nextTagIndex].focusedWindow = windowToMove;

        xcb_unmap_window(conn_.get(), windowToMove);

        rearrange_windows();
        bar_.update(current_tag_, tags_);

        if (!currentTag.windows.empty())
        {
            focus_window(currentTag.windows.back().id);
        }
    }
}

void WindowManager::launch_program(std::string const& command)
{
    if (fork() == 0)
    {
        setsid();
        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        exit(1);
    }
}

std::string WindowManager::get_window_name(xcb_window_t window)
{
    xcb_get_property_cookie_t cookie = xcb_get_property(conn_.get(), 0, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 1024);
    xcb_get_property_reply_t* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);

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

} // namespace lwm
