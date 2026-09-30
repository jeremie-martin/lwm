#pragma once
#include <unistd.h>
#include <utility>

namespace lwm::test {
struct TestFd
{
    int fd = -1;
    explicit TestFd(int value = -1)
        : fd(value)
    { }
    TestFd(TestFd const&) = delete;
    TestFd& operator=(TestFd const&) = delete;
    TestFd(TestFd&& other) noexcept
        : fd(std::exchange(other.fd, -1))
    { }
    ~TestFd() { reset(); }
    void reset()
    {
        if (fd >= 0)
            close(std::exchange(fd, -1));
    }
};
} // namespace lwm::test
