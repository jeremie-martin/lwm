#include "lwm/core/command.hpp"
#include "lwm/core/xproperty.hpp"
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
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection_{ xcb_connect(nullptr, nullptr), xcb_disconnect };
    xcb_window_t root_ = XCB_NONE;

public:
    Display()
    {
        if (xcb_connection_has_error(conn()))
            throw std::runtime_error("cannot connect to the X display");
        root_ = xcb_setup_roots_iterator(xcb_get_setup(conn())).data->root;
    }

    xcb_connection_t* conn() const { return connection_.get(); }
    xcb_window_t root() const { return root_; }
    xcb_atom_t atom(char const* name) const
    {
        auto reply = lwm::reply(xcb_intern_atom_reply(conn(), xcb_intern_atom(conn(), 0, std::strlen(name), name), nullptr));
        return reply ? reply->atom : XCB_NONE;
    }
    xcb_window_t owner() const
    {
        auto reply = lwm::reply(xcb_get_selection_owner_reply(conn(), xcb_get_selection_owner(conn(), selection), nullptr));
        return reply ? reply->owner : XCB_NONE;
    }
    // The screen's WM, if it is LWM, which names its owner window "lwm".
    xcb_window_t lwm_owner() const
    {
        auto window = owner();
        return window != XCB_NONE && text(window, name) == "lwm" ? window : XCB_NONE;
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

    // Declared after the connection, which initializes first.
    xcb_atom_t const selection = atom("WM_S0"), utf8 = atom("UTF8_STRING"), state = atom("_LWM_STATE"),
                       name = atom("_NET_WM_NAME");
};

// Sends one command from a private requester window and waits for its reply.
// A timeout leaves the outcome unknown: the WM may have run the command.
int run(std::string const& request, std::chrono::milliseconds timeout)
{
    Display x;
    auto* conn = x.conn();
    xcb_atom_t command = x.atom("_LWM_COMMAND"), reply = x.atom("_LWM_REPLY");
    auto wm = x.lwm_owner();
    if (wm == XCB_NONE || !x.select(wm, XCB_EVENT_MASK_STRUCTURE_NOTIFY))
        throw std::runtime_error("lwm is not running");
    xcb_window_t requester = xcb_generate_id(conn);
    uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(conn, 0, requester, x.root(), -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &events);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, requester, command, x.utf8, 8, request.size(), request.data());
    xcb_client_message_event_t message{ };
    message.response_type = XCB_CLIENT_MESSAGE;
    message.format = 32;
    message.window = wm;
    message.type = command;
    message.data.data32[0] = requester;
    // An empty mask delivers the message to the owner window's creator, the WM.
    xcb_send_event(conn, 0, wm, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char const*>(&message));
    xcb_flush(conn);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        while (auto event = lwm::reply(xcb_poll_for_event(conn)))
        {
            uint8_t type = event->response_type & ~0x80;
            auto const& property = reinterpret_cast<xcb_property_notify_event_t const&>(*event);
            if (type == XCB_DESTROY_NOTIFY && reinterpret_cast<xcb_destroy_notify_event_t const&>(*event).window == wm)
                throw std::runtime_error("lwm exited before replying");
            if (type != XCB_PROPERTY_NOTIFY || property.window != requester || property.atom != reply
                || property.state != XCB_PROPERTY_NEW_VALUE)
                continue;
            auto response = x.text(requester, reply, true).value_or("");
            if (response.starts_with("error "))
                throw std::runtime_error(response.substr(6));
            if (response != "ok" && !response.starts_with("ok "))
                throw std::runtime_error("invalid reply");
            if (response.starts_with("ok "))
                std::cout << response.substr(3) << '\n';
            std::cout.flush();
            return std::cout ? 0 : 1;
        }
        if (xcb_connection_has_error(conn))
            throw std::runtime_error("X connection closed");
        auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0)
            throw std::runtime_error("timed out waiting for lwm; the command may have run");
        pollfd descriptor{ xcb_get_file_descriptor(conn), POLLIN, 0 };
        poll(&descriptor, 1, static_cast<int>(remaining));
    }
}

// Prints the published state now and after every change, one JSON line each.
// A restart announces its successor with a MANAGER message on the root.
int watch()
{
    Display x;
    xcb_atom_t manager = x.atom("MANAGER");
    x.select(x.root(), XCB_EVENT_MASK_STRUCTURE_NOTIFY);
    auto wm = x.lwm_owner();
    if (wm == XCB_NONE)
        throw std::runtime_error("lwm is not running");
    std::string printed;
    for (bool attach = true;;)
    {
        // Select before reading, so no change can fall between them.
        if (attach && wm != XCB_NONE && !x.select(wm, XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY))
            wm = XCB_NONE;
        if (auto text = wm != XCB_NONE ? x.text(wm, x.state) : std::nullopt; text && !text->empty() && *text != printed)
        {
            printed = std::move(*text);
            std::cout << printed << '\n' << std::flush;
        }
        if (!std::cout)
            return 0; // A closed pipe ends watching normally.
        auto event = lwm::reply(xcb_wait_for_event(x.conn()));
        if (!event)
            throw std::runtime_error("X connection closed");
        uint8_t type = event->response_type & ~0x80;
        auto const& destroyed = reinterpret_cast<xcb_destroy_notify_event_t const&>(*event);
        auto const& message = reinterpret_cast<xcb_client_message_event_t const&>(*event);
        attach = (type == XCB_DESTROY_NOTIFY && destroyed.window == wm)
            || (type == XCB_CLIENT_MESSAGE && message.type == manager && message.data.data32[1] == x.selection);
        if (attach)
            wm = x.lwm_owner();
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
