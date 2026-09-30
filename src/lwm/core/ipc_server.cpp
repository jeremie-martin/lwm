#include "ipc_server.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace lwm::ipc {
namespace {
constexpr auto timeout = std::chrono::milliseconds(500);
constexpr size_t max_clients = 32;
constexpr size_t max_subscribers = 8;

void close_fd(int& fd)
{
    if (fd >= 0)
        close(fd);
    fd = -1;
}
}

void Server::start(std::string path)
{
    stop();
    sequence_ = 0;
    instance_ = std::to_string(getpid()) + "-" + std::to_string(Clock::now().time_since_epoch().count());
    if (path.size() >= sizeof(sockaddr_un::sun_path))
        throw std::runtime_error("IPC socket path is too long: " + path);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    clients_.reserve(max_clients + max_subscribers);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        throw std::runtime_error("Failed to create IPC socket");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), offsetof(sockaddr_un, sun_path) + path.size() + 1) < 0)
    {
        int error = errno;
        close(fd);
        throw std::runtime_error("Failed to bind IPC socket: " + std::string(std::strerror(error)));
    }
    listener_ = fd;
    path_ = std::move(path);
    if (listen(listener_, 16) < 0 || chmod(path_.c_str(), 0600) < 0)
    {
        int error = errno;
        stop();
        throw std::runtime_error("Failed to initialize IPC socket: " + std::string(std::strerror(error)));
    }
}

void Server::stop()
{
    for (auto& client : clients_) close_fd(client.fd);
    clients_.clear();
    close_fd(listener_);
    if (!path_.empty())
        unlink(path_.c_str());
    path_.clear();
}

std::optional<Server::Clock::time_point> Server::deadline() const
{
    std::optional<Clock::time_point> result;
    for (auto const& client : clients_)
        if (client.fd >= 0 && client.deadline && (!result || *client.deadline < *result))
            result = client.deadline;
    return result;
}

void Server::append_poll_fds(std::vector<pollfd>& fds) const
{
    fds.push_back({ listener_, POLLIN, 0 });
    for (auto const& client : clients_)
    {
        short events = client.reading ? POLLIN : client.output.empty() ? 0 : POLLOUT;
        fds.push_back({ client.fd, events, 0 });
    }
}

void Server::dispatch(std::span<pollfd const> fds, Handler const& handler)
{
    // Process the old snapshot before accepting new descriptors, which may reuse closed fd numbers.
    for (size_t i = 0; i < clients_.size(); ++i)
    {
        auto& client = clients_[i];
        auto const& ready = fds[i + 1];
        if (client.fd < 0 || ready.fd != client.fd)
            continue;
        if (ready.revents & (POLLERR | POLLNVAL))
            close_fd(client.fd);
        else if (client.reading && (ready.revents & (POLLIN | POLLHUP)))
            read_request(client, handler);
        else if (ready.revents & POLLOUT)
            write_response(client);
        else if (ready.revents & POLLHUP)
            close_fd(client.fd);
    }
    expire();
    if (fds[0].revents & POLLIN)
        accept_clients();
}

void Server::expire()
{
    auto now = Clock::now();
    for (auto& client : clients_)
        if (client.deadline && now >= *client.deadline)
            close_fd(client.fd);
    std::erase_if(clients_, [](auto const& client) { return client.fd < 0; });
}

void Server::accept_clients()
{
    // Bound accepts as well as reads/writes so the WM returns to X input regularly.
    for (int accepted = 0; accepted < 8; ++accepted)
    {
        int fd = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0)
            return;
        auto ordinary =
            std::count_if(clients_.begin(), clients_.end(), [](auto const& c) { return c.fd >= 0 && !c.mask; });
        if (ordinary >= max_clients || clients_.size() >= max_clients + max_subscribers)
        {
            constexpr std::string_view busy = "error busy\n";
            send(fd, busy.data(), busy.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            close(fd);
        }
        else
            clients_.push_back(Client{ fd, Clock::now() + timeout });
    }
}

void Server::read_request(Client& client, Handler const& handler)
{
    char buffer[512];
    while (true)
    {
        ssize_t received = recv(client.fd, buffer, sizeof(buffer), 0);
        if (received > 0)
        {
            std::string_view chunk(buffer, received);
            auto end = chunk.find('\n');
            client.input.append(chunk.substr(0, end));
            if (client.input.size() + 1 >= max_request_bytes)
            {
                respond(client, "error request too large");
                return;
            }
            if (end != chunk.npos)
            {
                execute(client, handler);
                return;
            }
        }
        else if (received == 0)
        {
            if (client.input.empty())
                close_fd(client.fd);
            else
                execute(client, handler);
            return;
        }
        else if (errno != EINTR)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                close_fd(client.fd);
            return;
        }
    }
}

void Server::execute(Client& client, Handler const& handler)
{
    auto command = parse_command(client.input);
    if (!command)
    {
        respond(client, "error " + command.error());
        return;
    }
    if (auto const* subscribe = std::get_if<Subscribe>(&*command))
    {
        auto subscribers =
            std::count_if(clients_.begin(), clients_.end(), [](auto const& c) { return c.fd >= 0 && c.mask; });
        if (subscribers >= max_subscribers)
            respond(client, "error max subscribers reached");
        else
        {
            // Register before acknowledgement. Subsequent events queue behind it.
            client.mask = subscribe->mask;
            ++subscriptions_;
            respond(client, "ok subscribed");
        }
    }
    else
        respond(client, handler(*command));
}

void Server::respond(Client& client, std::string response)
{
    if (response.size() + 1 > max_reply_bytes)
        response = "error response too large";
    response.push_back('\n');
    client.input.clear();
    client.reading = false;
    client.output = std::move(response);
    client.deadline = Clock::now() + timeout;
    write_response(client);
}

void Server::write_response(Client& client)
{
    size_t count = std::min<size_t>(client.output.size() - client.sent, 64 * 1024);
    ssize_t sent = send(client.fd, client.output.data() + client.sent, count, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            close_fd(client.fd);
        return;
    }
    client.sent += static_cast<size_t>(sent);
    if (client.mask && sent > 0)
        client.deadline = Clock::now() + timeout;
    if (client.sent == client.output.size())
    {
        if (!client.mask)
            close_fd(client.fd);
        else
        {
            client.output.clear();
            client.sent = 0;
            client.deadline.reset();
        }
    }
}

bool Server::has_subscribers(EventType type) const
{
    return std::ranges::any_of(clients_, [type](auto const& c) { return c.fd >= 0 && (c.mask & type); });
}

void Server::emit(EventType type, std::string_view json)
{
    if (!has_subscribers(type))
        return;
    // Event objects are constructed by the WM; sequence is additive wire metadata.
    std::string line = "{\"instance\":\"" + instance_ + "\",\"sequence\":" + std::to_string(++sequence_) + ","
        + std::string(json.substr(1)) + "\n";
    for (auto& client : clients_)
    {
        if (client.fd < 0 || !(client.mask & type))
            continue;
        if (client.output.size() - client.sent + line.size() > max_event_bytes)
        {
            close_fd(client.fd);
            continue;
        }
        if (client.output.empty())
            client.deadline = Clock::now() + timeout;
        if (client.sent)
        {
            client.output.erase(0, client.sent);
            client.sent = 0;
        }
        client.output += line;
    }
}

} // namespace lwm::ipc
