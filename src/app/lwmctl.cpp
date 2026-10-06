#include "lwm/core/ipc.hpp"
#include "lwm/core/command.hpp"
#include "lwm/core/xproperty.hpp"
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <optional>
#include <sys/time.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {
bool print_usage(std::ostream& out, std::string_view group = {})
{
    out << "usage: lwmctl [--socket PATH] [--timeout MS] [--] <command>\n\n";
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
    out << "\nTimeout defaults to 2000 ms. Use -- before arguments that resemble options.\n";
    return found;
}

// The running WM publishes its socket path on the root window.
std::optional<std::string> root_socket_path()
{
    int screen_index = 0;
    xcb_connection_t* conn = xcb_connect(nullptr, &screen_index);
    std::optional<std::string> value;
    if (xcb_connection_has_error(conn))
    {
        xcb_disconnect(conn);
        return value;
    }
    auto iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; iter.rem && i < screen_index; ++i) xcb_screen_next(&iter);
    if (iter.rem)
    {
        auto atom = [&](char const* name)
        {
            auto* reply = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 1, std::strlen(name), name), nullptr);
            xcb_atom_t result = reply ? reply->atom : XCB_NONE;
            free(reply);
            return result;
        };
        xcb_atom_t property = atom("_LWM_IPC_SOCKET"), utf8 = atom("UTF8_STRING");
        if (property != XCB_NONE)
            if (auto reply = lwm::xproperty::read(conn, iter.data->root, property, utf8, 4096);
                lwm::xproperty::complete(reply, utf8, 8))
            {
                std::string_view text(static_cast<char const*>(xcb_get_property_value(reply.get())), xcb_get_property_value_length(reply.get()));
                if (!text.empty() && !text.contains('\0'))
                    value = std::string(text);
            }
    }
    xcb_disconnect(conn);
    return value;
}

std::string resolve_socket_path(std::optional<std::string> const& cli_socket)
{
    if (cli_socket.has_value())
        return *cli_socket;

    if (char const* env_socket = std::getenv("LWM_SOCKET"))
        return env_socket;

    if (auto root_socket = root_socket_path())
        return *root_socket;

    return lwm::ipc::default_socket_path().string();
}

// One owner for the connection, operation deadlines and complete-line framing.
// Kernel timeouts use the remaining allowance, so partial I/O never renews it.
class Socket
{
public:
    explicit Socket(std::chrono::milliseconds timeout) : timeout_(timeout)
    {
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0)
            fail("create socket");
    }
    ~Socket() { close(fd_); }
    Socket(Socket const&) = delete;
    Socket& operator=(Socket const&) = delete;

    void connect(std::string const& path)
    {
        if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path) || path.find('\0') != path.npos)
            throw std::runtime_error("invalid socket path");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        auto deadline = Clock::now() + timeout_;
        do
        {
            limit(SO_SNDTIMEO, deadline);
            if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 || errno == EISCONN)
            {
                remaining_timeout(deadline);
                return;
            }
        } while (errno == EINTR);
        fail("connect to " + path);
    }

    void send(std::string request)
    {
        request += '\n';
        auto deadline = Clock::now() + timeout_;
        for (std::string_view remaining = request; !remaining.empty();)
        {
            limit(SO_SNDTIMEO, deadline);
            auto count = ::send(fd_, remaining.data(), remaining.size(), MSG_NOSIGNAL);
            if (count < 0)
            {
                if (errno == EINTR)
                    continue;
                fail("send request");
            }
            if (!count)
                throw std::runtime_error("connection closed during request");
            remaining.remove_prefix(static_cast<size_t>(count));
        }
        remaining_timeout(deadline);
    }

    // An idle subscription waits indefinitely for the next line to begin.
    std::optional<std::string> line(size_t limit, bool idle = false)
    {
        std::optional<Clock::time_point> deadline;
        for (;;)
        {
            auto end = buffer_.find('\n');
            if ((end == buffer_.npos ? buffer_.size() : end + 1) > limit)
                throw std::runtime_error("response line too large");
            if (end != buffer_.npos)
            {
                remaining_timeout(deadline);
                auto result = buffer_.substr(0, end);
                buffer_.erase(0, end + 1);
                return result;
            }
            if (!deadline && (!idle || !buffer_.empty()))
                deadline = (buffer_.empty() ? Clock::now() : received_at_) + timeout_;
            this->limit(SO_RCVTIMEO, deadline);
            std::array<char, 8192> bytes;
            auto count = recv(fd_, bytes.data(), bytes.size(), 0);
            if (count < 0)
            {
                if (errno == EINTR)
                    continue;
                fail("read response");
            }
            if (!count)
            {
                if (!buffer_.empty())
                    throw std::runtime_error("incomplete response line");
                return {};
            }
            received_at_ = Clock::now();
            if (!deadline)
                deadline = received_at_ + timeout_;
            buffer_.append(bytes.data(), static_cast<size_t>(count));
        }
    }

private:
    int fd_ = -1;
    using Clock = std::chrono::steady_clock;
    std::chrono::milliseconds timeout_;
    std::string buffer_;
    Clock::time_point received_at_; // A buffered next line began in the last read.

    static timeval remaining_timeout(std::optional<Clock::time_point> deadline)
    {
        timeval value{}; // Zero disables the kernel timeout for idle subscriptions.
        if (deadline)
        {
            auto remaining = std::chrono::ceil<std::chrono::microseconds>(*deadline - Clock::now()).count();
            if (remaining <= 0)
                throw std::runtime_error("socket operation timed out");
            value = { static_cast<time_t>(remaining / 1000000), static_cast<suseconds_t>(remaining % 1000000) };
        }
        return value;
    }
    void limit(int option, std::optional<Clock::time_point> deadline)
    {
        auto value = remaining_timeout(deadline);
        if (setsockopt(fd_, SOL_SOCKET, option, &value, sizeof(value)) < 0)
            fail("set socket timeout");
    }
    [[noreturn]] static void fail(std::string const& operation)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)
            throw std::runtime_error("socket operation timed out");
        throw std::runtime_error(operation + ": " + std::strerror(errno));
    }
};

// Prints the WM's published state now and after every change, one JSON line
// each. The state lives on the WM_S0 owner window, so it dies with the WM; a
// restart announces its successor with a MANAGER message on the root.
int watch()
{
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection(xcb_connect(nullptr, nullptr), xcb_disconnect);
    auto* conn = connection.get();
    if (xcb_connection_has_error(conn))
        throw std::runtime_error("cannot connect to the X display");
    auto root = xcb_setup_roots_iterator(xcb_get_setup(conn)).data->root;
    auto atom = [&](char const* name)
    {
        auto reply = lwm::reply(xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, std::strlen(name), name), nullptr));
        return reply ? reply->atom : XCB_NONE;
    };
    xcb_atom_t selection = atom("WM_S0"), manager = atom("MANAGER"), state = atom("_LWM_STATE"), utf8 = atom("UTF8_STRING");
    uint32_t root_events = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn, root, XCB_CW_EVENT_MASK, &root_events);
    auto owner = [&]
    {
        auto reply = lwm::reply(xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, selection), nullptr));
        return reply ? reply->owner : XCB_NONE;
    };
    xcb_window_t wm = owner();
    if (wm == XCB_NONE)
        throw std::runtime_error("lwm is not running");
    std::string printed;
    auto print = [&]
    {
        auto reply = lwm::xproperty::read(conn, wm, state, utf8, UINT32_MAX / 4);
        if (!lwm::xproperty::complete(reply, utf8, 8))
            return;
        std::string text(static_cast<char const*>(xcb_get_property_value(reply.get())), xcb_get_property_value_length(reply.get()));
        if (text.empty() || text == printed)
            return;
        printed = std::move(text);
        std::cout << printed << '\n' << std::flush;
    };
    for (bool attach = true;; )
    {
        // Select before reading, so no change can fall between them.
        uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        if (attach && wm != XCB_NONE)
            if (auto* error = xcb_request_check(conn, xcb_change_window_attributes_checked(conn, wm, XCB_CW_EVENT_MASK, &events)))
            {
                free(error);
                wm = XCB_NONE;
            }
        if (wm != XCB_NONE)
            print();
        if (!std::cout)
            return 0; // A closed pipe ends watching normally.
        auto event = lwm::reply(xcb_wait_for_event(conn));
        if (!event)
            throw std::runtime_error("X connection closed");
        uint8_t type = event->response_type & ~0x80;
        auto const& destroyed = reinterpret_cast<xcb_destroy_notify_event_t const&>(*event);
        auto const& message = reinterpret_cast<xcb_client_message_event_t const&>(*event);
        attach = (type == XCB_DESTROY_NOTIFY && destroyed.window == wm)
            || (type == XCB_CLIENT_MESSAGE && message.type == manager && message.data.data32[1] == selection);
        if (attach)
            wm = owner();
    }
}

int run(std::string const& path, std::string const& request, std::chrono::milliseconds timeout)
{
    Socket socket(timeout);
    socket.connect(path);
    socket.send(request);
    auto response = socket.line(lwm::ipc::max_reply_bytes);
    if (!response)
        throw std::runtime_error("connection closed before response");
    if (*response == "error" || response->starts_with("error "))
        throw std::runtime_error(response->size() > 6 ? response->substr(6) : "command failed");
    if (*response != "ok" && !response->starts_with("ok "))
        throw std::runtime_error("invalid response");
    if (socket.line(lwm::ipc::max_reply_bytes))
        throw std::runtime_error("multiple command replies");
    if (response->starts_with("ok "))
        std::cout << response->substr(3) << '\n';
    std::cout.flush();
    return std::cout ? 0 : 1;
}
} // namespace

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);
    try
    {
        std::optional<std::string> socket;
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
            if (options && (arg == "--socket" || arg == "--timeout"))
            {
                if (++i == argc)
                    throw std::runtime_error(std::string(arg) + " requires a value");
                if (arg == "--socket")
                    socket = argv[i];
                else
                {
                    std::string_view text = argv[i];
                    int value = 0;
                    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                    if (error != std::errc{} || end != text.data() + text.size() || value < 1 || value > 600000)
                        throw std::runtime_error("timeout must be 1–600000 milliseconds");
                    timeout = std::chrono::milliseconds(value);
                }
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
        return run(resolve_socket_path(socket), *request, timeout);
    }
    catch (std::exception const& error)
    {
        std::cerr << "lwmctl: " << error.what() << '\n';
        return 1;
    }
}
