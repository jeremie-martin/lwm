#include "lwm/core/events.hpp"
#include "lwm/core/ipc.hpp"
#include "lwm/core/ipc_command.hpp"
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Deadline = std::optional<Clock::time_point>;

bool print_usage(std::ostream& out, std::string_view group = {})
{
    out << "usage: lwmctl [--socket PATH] [--timeout MS] [--] <command>\n\n";
    bool found = false;
    for (auto const& spec : lwm::ipc::command_specs())
        if (group.empty() || spec.name == group || (spec.name.starts_with(group) && spec.name[group.size()] == ' '))
        {
            out << "  " << spec.usage << "\n      " << spec.description << '\n';
            found = true;
        }
    if (group.empty() || group == "subscribe")
    {
        out << "\nSubscription filters:";
        for (auto const& event : lwm::event_specs) out << ' ' << event.name;
        out << '\n';
    }
    out << "\nTimeout defaults to 2000 ms; idle subscriptions do not time out.\n"
           "Use -- before arguments that resemble options.\n";
    return found;
}

std::optional<std::string> root_socket_path()
{
    int screen_index = 0;
    xcb_connection_t* conn = xcb_connect(nullptr, &screen_index);
    if (!conn || xcb_connection_has_error(conn))
    {
        if (conn)
            xcb_disconnect(conn);
        return std::nullopt;
    }

    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; iter.rem && i < screen_index; ++i)
        xcb_screen_next(&iter);
    if (!iter.rem)
    {
        xcb_disconnect(conn);
        return std::nullopt;
    }

    auto value = lwm::ipc::get_root_text_property(conn, iter.data->root, "_LWM_IPC_SOCKET");
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

// One owner for connection, deadlines, buffering, and complete-line framing.
class Socket
{
public:
    explicit Socket(std::chrono::milliseconds timeout)
        : timeout_(timeout)
    { }
    ~Socket()
    {
        if (fd_ >= 0)
            close(fd_);
    }
    Socket(Socket const&) = delete;
    Socket& operator=(Socket const&) = delete;

    void connect_to(std::string const& path)
    {
        if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path) || path.find('\0') != path.npos)
            throw std::runtime_error("invalid socket path");
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0)
            fail("create socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        auto deadline = Clock::now() + timeout_;
        for (;;)
        {
            if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
                return;
            if (errno == EINTR)
                continue;
            if (errno != EINPROGRESS)
                fail("connect to " + path);
            wait(POLLOUT, deadline);
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
                fail("check connection");
            if (error)
            {
                errno = error;
                fail("connect to " + path);
            }
            return;
        }
    }

    void send(std::string request)
    {
        request += '\n';
        auto deadline = Clock::now() + timeout_;
        std::string_view remaining = request;
        while (!remaining.empty())
        {
            wait(POLLOUT, deadline);
            auto count = ::send(fd_, remaining.data(), remaining.size(), MSG_NOSIGNAL);
            if (count < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                fail("send request");
            }
            if (!count)
                throw std::runtime_error("connection closed while sending request");
            remaining.remove_prefix(count);
        }
    }

    std::optional<std::string> line(size_t limit, bool idle = false)
    {
        Deadline deadline = idle && buffer_.empty() ? Deadline{} : Deadline{ Clock::now() + timeout_ };
        for (;;)
        {
            auto end = buffer_.find('\n');
            if ((end == buffer_.npos ? buffer_.size() : end + 1) > limit)
                throw std::runtime_error("response line too large");
            if (end != buffer_.npos)
            {
                auto result = buffer_.substr(0, end);
                buffer_.erase(0, end + 1);
                return result;
            }
            wait(POLLIN, deadline);
            std::array<char, 8192> bytes;
            auto count = recv(fd_, bytes.data(), bytes.size(), 0);
            if (count < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                fail("read response");
            }
            if (!count)
            {
                if (!buffer_.empty())
                    throw std::runtime_error("incomplete response line");
                return {};
            }
            if (!deadline)
                deadline = Clock::now() + timeout_;
            buffer_.append(bytes.data(), count);
        }
    }

private:
    int fd_ = -1;
    std::chrono::milliseconds timeout_;
    std::string buffer_;
    [[noreturn]] static void fail(std::string const& operation)
    {
        throw std::runtime_error(operation + ": " + std::strerror(errno));
    }
    void wait(short events, Deadline deadline)
    {
        for (;;)
        {
            int milliseconds = -1;
            if (deadline)
            {
                auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now());
                if (remaining.count() <= 0)
                    throw std::runtime_error("socket operation timed out");
                milliseconds = static_cast<int>(remaining.count());
            }
            pollfd descriptor{ fd_, events, 0 };
            int result = poll(&descriptor, 1, milliseconds);
            if (result > 0)
                return; // recv/send report EOF and socket errors.
            if (result < 0 && errno != EINTR)
                fail("poll socket");
            if (!result)
                throw std::runtime_error("socket operation timed out");
        }
    }
};

int run(std::string const& path, std::string const& request, bool subscribe, std::chrono::milliseconds timeout)
{
    Socket socket(timeout);
    socket.connect_to(path);
    socket.send(request);
    auto response = socket.line(lwm::ipc::max_reply_bytes);
    if (!response)
        throw std::runtime_error("connection closed before response");
    if (*response == "error" || response->starts_with("error "))
        throw std::runtime_error(response->size() > 6 ? response->substr(6) : "command failed");
    if (subscribe)
    {
        if (*response != "ok subscribed")
            throw std::runtime_error("invalid subscription response");
        while (auto event = socket.line(lwm::ipc::max_event_bytes, true))
        {
            std::cout << *event << '\n' << std::flush;
            if (!std::cout)
                return 0; // A closed pipe ends a subscription normally.
        }
        return 0;
    }
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
        auto request = lwm::ipc::encode_command(arguments);
        if (!request)
            throw std::runtime_error(request.error());
        return run(resolve_socket_path(socket), *request, arguments.front() == "subscribe", timeout);
    }
    catch (std::exception const& error)
    {
        std::cerr << "lwmctl: " << error.what() << '\n';
        return 1;
    }
}
