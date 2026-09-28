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
constexpr size_t max_request = 4096;
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
    if (path.size() >= sizeof(sockaddr_un::sun_path))
        throw std::runtime_error("IPC socket path is too long: " + path);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
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
    close_client();
    for (auto& subscriber : subscribers_) close_fd(subscriber.fd);
    subscribers_.clear();
    close_fd(listener_);
    if (!path_.empty())
        unlink(path_.c_str());
    path_.clear();
}

void Server::close_client()
{
    if (client_)
        close_fd(client_->fd);
    client_.reset();
}

std::optional<Server::Clock::time_point> Server::deadline() const
{
    return client_ ? std::optional{ client_->deadline } : std::nullopt;
}

void Server::append_poll_fds(std::vector<pollfd>& fds) const
{
    fds.push_back({ listener_, POLLIN, 0 });
    short events = client_ && std::holds_alternative<Response>(client_->state) ? POLLOUT : POLLIN;
    fds.push_back({ client_ ? client_->fd : -1, events, 0 });
    for (auto const& subscriber : subscribers_) fds.push_back({ subscriber.fd, 0, 0 });
}

void Server::dispatch(std::span<pollfd const> fds, Handler const& handler)
{
    // Consume the old poll snapshot before accepting/promoting any connections.
    for (size_t i = 2; i < fds.size(); ++i)
        if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
            for (auto& subscriber : subscribers_)
                if (subscriber.fd == fds[i].fd)
                    close_fd(subscriber.fd);
    std::erase_if(subscribers_, [](auto const& s) { return s.fd < 0; });
    if (fds[0].revents & POLLIN)
        accept_client();
    if (client_ && fds[1].fd == client_->fd)
    {
        if (fds[1].revents & (POLLERR | POLLNVAL))
            close_client();
        else if (std::holds_alternative<Request>(client_->state) && (fds[1].revents & (POLLIN | POLLHUP)))
            read_request(handler);
        else if (std::holds_alternative<Response>(client_->state) && (fds[1].revents & (POLLOUT | POLLHUP)))
            write_response();
    }
    expire();
}

void Server::expire()
{
    if (client_ && Clock::now() >= client_->deadline)
        close_client();
}

void Server::accept_client()
{
    int fd = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0)
        return;
    if (client_)
    {
        constexpr std::string_view busy = "error busy\n";
        send(fd, busy.data(), busy.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        close(fd);
        return;
    }
    client_.emplace(Client{ fd, Clock::now() + timeout });
}

void Server::read_request(Handler const& handler)
{
    auto& request = std::get<Request>(client_->state).text;
    char buffer[512];
    while (true)
    {
        ssize_t received = recv(client_->fd, buffer, sizeof(buffer), 0);
        if (received > 0)
        {
            request.append(buffer, received);
            if (request.size() >= max_request)
            {
                respond("error request too large");
                return;
            }
            if (auto end = request.find('\n'); end != std::string::npos)
            {
                request.resize(end);
                execute(std::move(request), handler);
                return;
            }
        }
        else if (received == 0)
        {
            if (request.empty())
                close_client();
            else
                execute(std::move(request), handler);
            return;
        }
        else if (errno != EINTR)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                close_client();
            return;
        }
    }
}

void Server::execute(std::string request, Handler const& handler)
{
    auto first = request.find_first_not_of(" \t\r\n\v\f");
    auto last = request.find_last_not_of(" \t\r\n\v\f");
    request = first == std::string::npos ? "" : request.substr(first, last - first + 1);
    if (request == "subscribe" || request.starts_with("subscribe "))
    {
        if (subscribers_.size() >= max_subscribers)
            respond("error max subscribers reached");
        else
        {
            std::string_view filter = request.size() > 9 ? std::string_view(request).substr(10) : std::string_view{};
            auto first = filter.find_first_not_of(" \t\r\n\v\f");
            filter = first == std::string_view::npos ? std::string_view{} : filter.substr(first);
            auto mask = parse_event_filter(filter);
            if (mask == 0)
                respond("error no recognized event types in filter");
            else
                respond("ok subscribed", mask);
        }
    }
    else
        respond(handler(request));
}

void Server::respond(std::string response, uint32_t subscribe_mask)
{
    response.push_back('\n');
    client_->state = Response{ std::move(response), 0, subscribe_mask };
    client_->deadline = Clock::now() + timeout;
    write_response();
}

void Server::write_response()
{
    auto& response = std::get<Response>(client_->state);
    // Bound work per dispatch so a large reply cannot monopolize input handling.
    size_t count = std::min<size_t>(response.text.size() - response.sent, 64 * 1024);
    ssize_t sent = send(client_->fd, response.text.data() + response.sent, count, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            close_client();
        return;
    }
    response.sent += static_cast<size_t>(sent);
    if (response.sent == response.text.size())
    {
        if (response.subscribe_mask != 0)
        {
            subscribers_.push_back({ client_->fd, response.subscribe_mask });
            client_->fd = -1;
        }
        close_client();
    }
}

void Server::emit(EventType type, std::string_view json)
{
    if (std::ranges::none_of(subscribers_, [type](auto const& s) { return s.fd >= 0 && (s.mask & type); }))
        return;
    std::string line(json);
    line.push_back('\n');
    for (auto& subscriber : subscribers_)
    {
        if (subscriber.fd < 0 || !(subscriber.mask & type))
            continue;
        ssize_t sent = send(subscriber.fd, line.data(), line.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if ((sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            || (sent >= 0 && static_cast<size_t>(sent) != line.size()))
            close_fd(subscriber.fd);
    }
}

} // namespace lwm::ipc
