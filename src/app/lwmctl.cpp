#include "lwm/core/command.hpp"
#include "lwm/core/xproperty.hpp"
#include "lwm/core/xconnection.hpp"
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <xcb/xcbext.h>

namespace {
bool print_usage(std::ostream& out, std::string_view group = {})
{
    out << "usage: lwmctl [--timeout MS] [--] <command>\n\n";
    bool found = false;
    for (auto const& spec : lwm::command::command_specs())
        if (group.empty() || spec.name == group || (spec.name.starts_with(group) && spec.name[group.size()] == ' '))
        {
            out << "  " << spec.usage << "\n      " << spec.description << '\n';
            found = true;
        }
    if (group.empty() || group == "watch")
    {
        out << "  watch\n      print the state as JSON now and after every change\n";
        found = true;
    }
    out << "\nThe timeout bounds a command's reply (default 2000 ms). Use -- before arguments\n"
           "that resemble options.\n";
    return found;
}

// The running WM is the owner of the WM_S0 selection. Its owner window receives
// commands and carries the published state, so both die with the WM.
class Display
{
    decltype(lwm::connect_display()) connection_ = lwm::connect_display();
    xcb_window_t root_ = XCB_NONE;

public:
    Display()
    {
        root_ = xcb_setup_roots_iterator(xcb_get_setup(conn())).data->root;
        constexpr std::array names{ "WM_S0", "UTF8_STRING", "_LWM_STATE", "_NET_WM_NAME", "_LWM_COMMAND", "_LWM_REPLY", "MANAGER" };
        std::array atoms{ &selection, &utf8, &state, &name, &command, &response, &manager };
        std::array<xcb_intern_atom_cookie_t, names.size()> cookies;
        for (size_t i = 0; i < names.size(); ++i)
            cookies[i] = xcb_intern_atom(conn(), 0, std::strlen(names[i]), names[i]);
        for (size_t i = 0; i < names.size(); ++i)
        {
            auto reply = lwm::reply(xcb_intern_atom_reply(conn(), cookies[i], nullptr));
            *atoms[i] = reply ? reply->atom : XCB_NONE;
        }
    }

    xcb_connection_t* conn() const { return connection_.get(); }
    xcb_window_t root() const { return root_; }
    xcb_window_t owner() const
    {
        auto reply = lwm::reply(xcb_get_selection_owner_reply(conn(), xcb_get_selection_owner(conn(), selection), nullptr));
        return reply ? reply->owner : XCB_NONE;
    }
    // Subscribe before reading identity; its reply also settles the checked request.
    xcb_window_t lwm_owner(uint32_t events) const
    {
        auto window = owner();
        if (window == XCB_NONE)
            return XCB_NONE;
        auto selected = xcb_change_window_attributes_checked(conn(), window, XCB_CW_EVENT_MASK, &events);
        auto identity = text(window, name);
        auto error = lwm::reply(xcb_request_check(conn(), selected));
        return !error && identity == "lwm" ? window : XCB_NONE;
    }
    // Selects events on a window; false if it no longer exists.
    bool select(xcb_window_t window, uint32_t events) const
    {
        auto* error = xcb_request_check(conn(), xcb_change_window_attributes_checked(conn(), window, XCB_CW_EVENT_MASK, &events));
        free(error);
        return !error;
    }
    std::optional<std::string> text(xcb_window_t window, xcb_atom_t property, bool remove = false) const
    {
        return lwm::xproperty::text(conn(), window, property, utf8, remove);
    }

    xcb_atom_t selection = XCB_NONE, utf8 = XCB_NONE, state = XCB_NONE, name = XCB_NONE,
               command = XCB_NONE, response = XCB_NONE, manager = XCB_NONE;
};

// Sends one command from a private requester window and waits for its reply.
// A timeout leaves the outcome unknown: the WM may have run the command.
int run(std::string const& request, std::chrono::milliseconds timeout)
{
    Display x;
    auto* conn = x.conn();
    auto wm = x.lwm_owner(XCB_EVENT_MASK_STRUCTURE_NOTIFY);
    if (wm == XCB_NONE)
        throw std::runtime_error("lwm is not running");
    xcb_window_t requester = xcb_generate_id(conn);
    uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(conn, 0, requester, x.root(), -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &events);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, requester, x.command, x.utf8, 8, request.size(), request.data());
    xcb_client_message_event_t message{ };
    message.response_type = XCB_CLIENT_MESSAGE;
    message.format = 32;
    message.window = wm;
    message.type = x.command;
    message.data.data32[0] = requester;
    // An empty mask delivers the message to the owner window's creator, the WM.
    xcb_send_event(conn, 0, wm, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char const*>(&message));
    xcb_flush(conn);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::optional<xcb_get_property_cookie_t> pending;
    auto remaining = [&]
    {
        auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (milliseconds <= 0)
            throw std::runtime_error("timed out waiting for lwm; the command may have run");
        return static_cast<int>(milliseconds);
    };
    auto observe = [&](xcb_generic_event_t const& event)
    {
        remaining();
        uint8_t type = event.response_type & ~0x80;
        auto const& property = reinterpret_cast<xcb_property_notify_event_t const&>(event);
        if (!pending && type == XCB_DESTROY_NOTIFY && reinterpret_cast<xcb_destroy_notify_event_t const&>(event).window == wm)
            throw std::runtime_error("lwm exited before replying");
        if (!pending && type == XCB_PROPERTY_NOTIFY && property.window == requester && property.atom == x.response
            && property.state == XCB_PROPERTY_NEW_VALUE)
        {
            pending = xcb_get_property(conn, true, requester, x.response, x.utf8, 0, UINT32_MAX / 4);
            xcb_flush(conn);
        }
    };
    for (;;)
    {
        remaining();
        while (auto event = lwm::reply(xcb_poll_for_event(conn)))
            observe(*event);
        void* value = nullptr;
        if (pending && xcb_poll_for_reply(conn, pending->sequence, &value, nullptr))
        {
            auto property = lwm::reply(static_cast<xcb_get_property_reply_t*>(value));
            if (!lwm::xproperty::complete(property, x.utf8, 8))
                throw std::runtime_error("invalid reply");
            std::string response(static_cast<char const*>(xcb_get_property_value(property.get())),
                                 static_cast<size_t>(xcb_get_property_value_length(property.get())));
            if (response.starts_with("error "))
                throw std::runtime_error(response.substr(6));
            if (response != "ok" && !response.starts_with("ok "))
                throw std::runtime_error("invalid reply");
            if (response.starts_with("ok "))
                std::cout << response.substr(3) << '\n';
            std::cout.flush();
            return std::cout ? 0 : 1;
        }
        // Reply polling can read lifecycle events into XCB's queue as well.
        if (auto event = lwm::reply(xcb_poll_for_queued_event(conn)))
        {
            observe(*event);
            continue;
        }
        if (xcb_connection_has_error(conn))
            throw std::runtime_error("X connection closed");
        pollfd descriptor{ xcb_get_file_descriptor(conn), POLLIN, 0 };
        poll(&descriptor, 1, remaining());
    }
}

// Prints the published state now and after every change, one JSON line each.
// A restart announces its successor with a MANAGER message on the root.
int watch()
{
    Display x;
    x.select(x.root(), XCB_EVENT_MASK_STRUCTURE_NOTIFY);
    constexpr uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    auto wm = x.lwm_owner(events);
    if (wm == XCB_NONE)
        throw std::runtime_error("lwm is not running");
    std::string printed;
    for (;;)
    {
        if (auto text = wm != XCB_NONE ? x.text(wm, x.state) : std::nullopt; text && !text->empty() && *text != printed)
        {
            printed = std::move(*text);
            std::cout << printed << '\n' << std::flush;
        }
        if (!std::cout)
            return 0; // A closed pipe ends watching normally.
        bool attach = false;
        for (bool changed = false; !attach && !changed;)
        {
            auto event = lwm::reply(xcb_wait_for_event(x.conn()));
            if (!event)
                throw std::runtime_error("X connection closed");
            do
            {
                uint8_t type = event->response_type & ~0x80;
                auto const& destroyed = reinterpret_cast<xcb_destroy_notify_event_t const&>(*event);
                auto const& message = reinterpret_cast<xcb_client_message_event_t const&>(*event);
                attach |= (type == XCB_DESTROY_NOTIFY && destroyed.window == wm)
                    || (type == XCB_CLIENT_MESSAGE && message.type == x.manager && message.data.data32[1] == x.selection);
                auto const& property = reinterpret_cast<xcb_property_notify_event_t const&>(*event);
                changed |= type == XCB_PROPERTY_NOTIFY && property.window == wm && property.atom == x.state;
                event = lwm::reply(xcb_poll_for_queued_event(x.conn()));
            } while (event);
            if (xcb_connection_has_error(x.conn()))
                throw std::runtime_error("X connection closed");
        }
        if (attach)
            wm = x.lwm_owner(events);
    }
}
} // namespace

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);
    try
    {
        std::chrono::milliseconds timeout{ 2000 };
        std::vector<std::string> arguments;
        bool options = true;
        bool help = false;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view arg = argv[i];
            if (options && arg == "--")
            {
                options = false;
                continue;
            }
            if (options && (arg == "--help" || arg == "-h"))
            {
                help = true;
                continue;
            }
            if (options && arg == "--timeout")
            {
                if (++i == argc)
                    throw std::runtime_error("--timeout requires a value");
                std::string_view text = argv[i];
                int value = 0;
                auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                if (error != std::errc{} || end != text.data() + text.size() || value < 1 || value > 600000)
                    throw std::runtime_error("timeout must be 1–600000 milliseconds");
                timeout = std::chrono::milliseconds(value);
                continue;
            }
            arguments.emplace_back(arg);
        }
        if (help)
        {
            std::string group;
            for (auto const& arg : arguments)
            {
                if (!group.empty())
                    group += ' ';
                group += arg;
            }
            return print_usage(std::cout, group) ? 0 : 1;
        }
        if (arguments.empty())
        {
            print_usage(std::cerr);
            return 1;
        }
        if (arguments == std::vector<std::string>{ "watch" })
            return watch();
        auto request = lwm::command::encode_command(arguments);
        if (!request)
            throw std::runtime_error(request.error());
        return run(*request, timeout);
    }
    catch (std::exception const& error)
    {
        std::cerr << "lwmctl: " << error.what() << '\n';
        return 1;
    }
}
