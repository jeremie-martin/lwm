#include <csignal>
#include <unistd.h>

int main(int, char** argv)
{
    raise(SIGSTOP);
    // Hand off as LWM itself: a successor restarts from its argv[0].
    argv[0] = const_cast<char*>(LWM_BINARY_PATH);
    execv(LWM_BINARY_PATH, argv);
    return 127;
}
