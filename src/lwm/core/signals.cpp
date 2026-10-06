#include "signals.hpp"
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

namespace lwm {
namespace {
sig_atomic_t volatile reload_fd = -1;
void reload(int)
{
    int saved = errno;
    char byte = 1;
    if (reload_fd >= 0)
    {
        // A full pipe already represents a pending reload.
        while (write(reload_fd, &byte, 1) < 0 && errno == EINTR)
        { }
    }
    errno = saved;
}
}

SignalPipe::SignalPipe()
{
    if (reload_fd >= 0)
        throw std::logic_error("Process signal handlers already owned");
    if (pipe2(pipe_, O_CLOEXEC | O_NONBLOCK) < 0)
        throw std::system_error(errno, std::generic_category(), "Create signal pipe");
    bool hup_installed = false;
    try
    {
        struct sigaction action{ };
        action.sa_handler = reload;
        action.sa_flags = SA_RESTART;
        sigemptyset(&action.sa_mask);
        reload_fd = pipe_[1];
        if (sigaction(SIGHUP, &action, &previous_hup_) < 0)
            throw std::system_error(errno, std::generic_category(), "Install SIGHUP handler");
        hup_installed = true;
        // The kernel reaps launched processes. Exec clears the flag, so launched
        // programs and a restarted WM do not inherit it.
        action.sa_handler = SIG_DFL;
        action.sa_flags = SA_NOCLDWAIT;
        if (sigaction(SIGCHLD, &action, &previous_child_) < 0)
            throw std::system_error(errno, std::generic_category(), "Install SIGCHLD handler");
        // Children that exited before installation, such as during exec, remain zombies.
        while (waitpid(-1, nullptr, WNOHANG) > 0)
        { }
    }
    catch (...)
    {
        if (hup_installed)
            sigaction(SIGHUP, &previous_hup_, nullptr);
        reload_fd = -1;
        close(pipe_[0]);
        close(pipe_[1]);
        throw;
    }
}
SignalPipe::~SignalPipe()
{
    sigaction(SIGHUP, &previous_hup_, nullptr);
    sigaction(SIGCHLD, &previous_child_, nullptr);
    reload_fd = -1;
    close(pipe_[0]);
    close(pipe_[1]);
}
void SignalPipe::drain() const
{
    char buffer[64];
    for (;;)
    {
        auto count = read(pipe_[0], buffer, sizeof(buffer));
        if (count > 0 || (count < 0 && errno == EINTR))
            continue;
        break;
    }
}
} // namespace lwm
