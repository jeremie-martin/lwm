#pragma once

#include "command.hpp"
#include "ipc.hpp"
#include <chrono>
#include <functional>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lwm::ipc {

// Owns the local stream protocol. Commands execute synchronously on the WM's
// event loop; socket reads and writes never wait for a client.
class Server
{
public:
    using Clock = std::chrono::steady_clock;
    using Handler = std::function<std::string(command::Request const&)>;
    Server() = default;
    ~Server() { stop(); }
    Server(Server const&) = delete;
    Server& operator=(Server const&) = delete;

    void start(std::string path);
    void stop();
    std::string const& path() const { return path_; }
    std::optional<Clock::time_point> deadline() const;
    void append_poll_fds(std::vector<pollfd>& fds) const;
    void dispatch(std::span<pollfd const> fds, Handler const& handler);
    void expire();

private:
    struct Client
    {
        int fd;
        std::optional<Clock::time_point> deadline;
        std::string input;
        std::string output;
        size_t sent = 0;
        // A client sends one request and closes after its reply.
        bool reading() const { return output.empty(); }
    };
    std::string path_;
    int listener_ = -1;
    std::vector<Client> clients_;

    void accept_clients();
    void read_request(Client& client, Handler const& handler);
    void execute(Client& client, Handler const& handler);
    void respond(Client& client, std::string response);
    void write_response(Client& client);
};

} // namespace lwm::ipc
