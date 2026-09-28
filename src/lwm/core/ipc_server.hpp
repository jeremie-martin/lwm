#pragma once

#include "events.hpp"
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
    using Handler = std::function<std::string(std::string const&)>;
    Server() = default;
    ~Server() { stop(); }
    Server(Server const&) = delete;
    Server& operator=(Server const&) = delete;

    void start(std::string path);
    void stop();
    std::string const& path() const { return path_; }
    bool has_subscribers() const { return !subscribers_.empty(); }
    std::optional<Clock::time_point> deadline() const;
    void append_poll_fds(std::vector<pollfd>& fds) const;
    void dispatch(std::span<pollfd const> fds, Handler const& handler);
    void expire();
    void emit(EventType type, std::string_view json);

private:
    struct Request
    {
        std::string text;
    };
    struct Response
    {
        std::string text;
        size_t sent = 0;
        uint32_t subscribe_mask = 0;
    };
    struct Client
    {
        int fd;
        Clock::time_point deadline;
        std::variant<Request, Response> state = Request{};
    };
    struct Subscriber
    {
        int fd;
        uint32_t mask;
    };
    std::string path_;
    int listener_ = -1;
    std::optional<Client> client_;
    std::vector<Subscriber> subscribers_;

    void accept_client();
    void close_client();
    void read_request(Handler const& handler);
    void execute(std::string request, Handler const& handler);
    void respond(std::string response, uint32_t subscribe_mask = 0);
    void write_response();
};

} // namespace lwm::ipc
