#pragma once
#include <signal.h>

namespace lwm {
// Process-scoped: survives failed WM construction and exec recovery. Construct
// before starting worker threads; destroy after joining them.
class SignalPipe
{
public:
    SignalPipe();
    ~SignalPipe();
    SignalPipe(SignalPipe const&) = delete;
    SignalPipe& operator=(SignalPipe const&) = delete;
    int fd() const { return pipe_[0]; }
    void drain() const;

private:
    int pipe_[2]{ -1, -1 };
    struct sigaction previous_hup_{ };
    struct sigaction previous_child_{ };
};
} // namespace lwm
