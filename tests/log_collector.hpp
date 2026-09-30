#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace lwm::test {
// A private native-journal endpoint. No test messages enter the user's journal.
class LogCollector
{
public:
    int fd = -1;
    std::string path;
    mutable std::string text;
    explicit LogCollector(std::string socket_path)
        : path(std::move(socket_path))
    {
        sockaddr_un address{ };
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path))
            throw std::runtime_error("log collector socket");
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            throw std::runtime_error("log collector socket");
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            close(fd);
            fd = -1;
            throw std::runtime_error("log collector bind");
        }
    }
    ~LogCollector()
    {
        if (fd >= 0)
        {
            close(fd);
            unlink(path.c_str());
        }
    }
    LogCollector(LogCollector const&) = delete;
    LogCollector& operator=(LogCollector const&) = delete;
    LogCollector(LogCollector&& other) noexcept
        : fd(std::exchange(other.fd, -1))
        , path(std::move(other.path))
        , text(std::move(other.text))
    { }
    LogCollector& operator=(LogCollector&& other) noexcept
    {
        if (this != &other)
        {
            if (fd >= 0)
            {
                close(fd);
                unlink(path.c_str());
            }
            fd = std::exchange(other.fd, -1);
            path = std::move(other.path);
            text = std::move(other.text);
        }
        return *this;
    }
    std::vector<std::map<std::string, std::string>> drain() const
    {
        std::vector<std::map<std::string, std::string>> records;
        std::array<char, 64 * 1024> buffer{ };
        ssize_t n;
        while ((n = recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT | MSG_TRUNC)) > 0)
        {
            if (static_cast<size_t>(n) > buffer.size())
                throw std::runtime_error("record exceeds test collector capacity");
            std::string_view remaining(buffer.data(), n);
            std::map<std::string, std::string> fields;
            while (!remaining.empty())
            {
                size_t newline = remaining.find('\n');
                if (newline == remaining.npos)
                    throw std::runtime_error("malformed journal field");
                auto key = remaining.substr(0, newline);
                remaining.remove_prefix(newline + 1);
                size_t equal = key.find('=');
                if (equal != key.npos)
                    fields.emplace(key.substr(0, equal), key.substr(equal + 1));
                else
                {
                    if (remaining.size() < 8)
                        throw std::runtime_error("missing journal length");
                    uint64_t size = 0;
                    for (int i = 0; i < 8; ++i) size |= uint64_t(static_cast<unsigned char>(remaining[i])) << (8 * i);
                    remaining.remove_prefix(8);
                    if (size >= remaining.size() || remaining[size] != '\n')
                        throw std::runtime_error("invalid journal length");
                    fields.emplace(key, remaining.substr(0, size));
                    remaining.remove_prefix(size + 1);
                }
            }
            text += fields.at("MESSAGE") + '\n';
            records.push_back(std::move(fields));
        }
        return records;
    }
    std::string diagnostics() const
    {
        drain();
        return text;
    }
};
}
