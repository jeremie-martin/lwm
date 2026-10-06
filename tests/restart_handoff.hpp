#pragma once

#include "wm_observations.hpp"

namespace lwm::test {

// Stop after the old WM has prepared its handoff and exec'd. The test remains
// the process's parent, so waitpid establishes the boundary without sleeps.
class PausedRestart
{
public:
    explicit PausedRestart(LwmProcess const& wm)
        : pid_(wm.pid())
    {
        ipc_ok("exec " LWM_RESTART_GATE_PATH);
        REQUIRE(wait_for_condition(
            [&]
            {
                int status = 0;
                auto result = waitpid(pid_, &status, WUNTRACED | WNOHANG);
                if (result == 0)
                    return false;
                REQUIRE(result == pid_);
                REQUIRE(WIFSTOPPED(status));
                return true;
            },
            std::chrono::seconds(3)
        ));
    }
    PausedRestart(PausedRestart const&) = delete;
    PausedRestart& operator=(PausedRestart const&) = delete;
    ~PausedRestart()
    {
        if (pid_ > 0)
            kill(pid_, SIGCONT);
    }
    void resume()
    {
        REQUIRE(kill(pid_, SIGCONT) == 0);
        pid_ = -1;
    }

private:
    pid_t pid_;
};

} // namespace lwm::test
