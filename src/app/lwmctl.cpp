#include "lwm/core/command.hpp"
#include "lwm/core/xproperty.hpp"
#include "lwm/core/xconnection.hpp"
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>

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
    out << "\nThe timeout bounds waiting for LWM's reply (default 2000 ms). Use -- before arguments\n"
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
        using namespace lwm::command;
        root_ = xcb_setup_roots_iterator(xcb_get_setup(conn())).data->root;
        std::pair<char const*, xcb_atom_t*> const names[] = { { "WM_S0", &selection }, { "UTF8_STRING", &utf8 },
            { state_atom, &state }, { "_NET_WM_NAME", &name }, { command_atom, &command }, { reply_atom, &response },
            { "MANAGER", &manager } };
        lwm::intern_atoms(conn(), names);
    }

    xcb_connection_t* conn() const { return connection_.get(); }
    xcb_window_t root() const { return root_; }
    // Subscribe before reading identity; its reply also settles the checked request.
    xcb_window_t lwm_owner(uint32_t events) const
    {
        auto owner = lwm::reply(xcb_get_selection_owner_reply(conn(), xcb_get_selection_owner(conn(), selection), nullptr));
        auto window = owner ? owner->owner : XCB_NONE;
        if (window == XCB_NONE)
            return XCB_NONE;
        auto selected = xcb_change_window_attributes_checked(conn(), window, XCB_CW_EVENT_MASK, &events);
        auto identity = text(window, name);
        auto error = lwm::reply(xcb_request_check(conn(), selected));
        return !error && identity == lwm::command::identity ? window : XCB_NONE;
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
    lwm::send_message(conn, wm, x.command, XCB_EVENT_MASK_NO_EVENT, { .data32 = { requester } });
    xcb_flush(conn);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        // Checked per event, so unrelated traffic cannot extend the wait.
        auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0)
            throw std::runtime_error("timed out waiting for lwm; the command may have run");
        auto event = lwm::reply(xcb_poll_for_event(conn));
        if (!event)
        {
            if (xcb_connection_has_error(conn))
                throw std::runtime_error("X connection closed");
            pollfd descriptor{ xcb_get_file_descriptor(conn), POLLIN, 0 };
            poll(&descriptor, 1, static_cast<int>(remaining));
            continue;
        }
        uint8_t type = event->response_type & ~0x80;
        auto const& property = reinterpret_cast<xcb_property_notify_event_t const&>(*event);
        if (type == XCB_DESTROY_NOTIFY && reinterpret_cast<xcb_destroy_notify_event_t const&>(*event).window == wm)
            throw std::runtime_error("lwm exited before replying");
        if (type != XCB_PROPERTY_NOTIFY || property.window != requester || property.atom != x.response
            || property.state != XCB_PROPERTY_NEW_VALUE)
            continue;
        auto response = x.text(requester, x.response, true).value_or("");
        if (response.starts_with("error "))
            throw std::runtime_error(response.substr(6));
        if (response != "ok" && !response.starts_with("ok "))
            throw std::runtime_error("invalid reply");
        if (response.starts_with("ok "))
            std::cout << response.substr(3) << '\n';
        std::cout.flush();
        return std::cout ? 0 : 1;
    }
}

// Prints the published state now and after every change, one JSON line each.
// A restart announces its successor with a MANAGER message on the root.
int watch()
{
    Display x;
    uint32_t const root_events = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(x.conn(), x.root(), XCB_CW_EVENT_MASK, &root_events);
    constexpr uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    auto wm = x.lwm_owner(events);
    if (wm == XCB_NONE)
        throw std::runtime_error("lwm is not running");
    std::string printed;
    for (bool changed = true, attach = false;;)
    {
        auto text = (changed || attach) && wm != XCB_NONE ? x.text(wm, x.state) : std::nullopt;
        if (text && !text->empty() && *text != printed)
        {
            printed = std::move(*text);
            std::cout << printed << '\n' << std::flush;
        }
        if (!std::cout)
            return 0; // A closed pipe ends watching normally.
        // One read covers a burst: every event already queued is consumed with it. Only a
        // state change or a new owner needs a read; the owner's other properties do not.
        changed = attach = false;
        for (auto event = lwm::reply(xcb_wait_for_event(x.conn())); event; event = lwm::reply(xcb_poll_for_queued_event(x.conn())))
        {
            uint8_t type = event->response_type & ~0x80;
            auto const& destroyed = reinterpret_cast<xcb_destroy_notify_event_t const&>(*event);
            auto const& message = reinterpret_cast<xcb_client_message_event_t const&>(*event);
            auto const& property = reinterpret_cast<xcb_property_notify_event_t const&>(*event);
            changed |= type == XCB_PROPERTY_NOTIFY && property.atom == x.state;
            attach |= (type == XCB_DESTROY_NOTIFY && destroyed.window == wm)
                || (type == XCB_CLIENT_MESSAGE && message.type == x.manager && message.data.data32[1] == x.selection);
        }
        if (xcb_connection_has_error(x.conn()))
            throw std::runtime_error("X connection closed");
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
        std::string request;
        size_t argument_count = 0;
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
            ++argument_count;
            if (!request.empty())
                request += ' ';
            request += arg;
        }
        if (help)
            return print_usage(std::cout, request) ? 0 : 1;
        if (argument_count == 0)
        {
            print_usage(std::cerr);
            return 1;
        }
        if (argument_count == 1 && request == "watch")
            return watch();
        if (auto parsed = lwm::command::parse_command(request); !parsed)
            throw std::runtime_error(parsed.error());
        return run(request, timeout);
    }
    catch (std::exception const& error)
    {
        std::cerr << "lwmctl: " << error.what() << '\n';
        return 1;
    }
}
