#pragma once

#include "events.hpp"
#include "ipc_command.hpp"
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
    using Handler = std::function<std::string(Request const&)>;
    Server() = default;
    ~Server() { stop(); }
    Server(Server const&) = delete;
    Server& operator=(Server const&) = delete;

    void start(std::string path);
    void stop();
    std::string const& path() const { return path_; }
    bool has_subscribers(EventType type) const;
    std::optional<Clock::time_point> deadline() const;
    void append_poll_fds(std::vector<pollfd>& fds) const;
    void dispatch(std::span<pollfd const> fds, Handler const& handler);
    void expire();
    void emit(EventType type, std::string_view json);
    uint64_t sequence() const { return sequence_; }
    // Counts accepted subscriptions, so the owner can take a baseline for new subscribers.
    uint64_t subscriptions() const { return subscriptions_; }
    std::string const& instance() const { return instance_; }

private:
    struct Client
    {
        int fd;
        std::optional<Clock::time_point> deadline;
        std::string input;
        std::string output;
        size_t sent = 0;
        uint32_t mask = 0;
        bool reading = true;
    };
    std::string path_;
    int listener_ = -1;
    std::vector<Client> clients_;
    uint64_t sequence_ = 0;
    uint64_t subscriptions_ = 0;
    std::string instance_;

    void accept_clients();
    void read_request(Client& client, Handler const& handler);
    void execute(Client& client, Handler const& handler);
    void respond(Client& client, std::string response);
    void write_response(Client& client);
};

} // namespace lwm::ipc
