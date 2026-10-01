#include <csignal>
#include <unistd.h>

int main(int, char** argv)
{
    raise(SIGSTOP);
    execv(LWM_BINARY_PATH, argv);
    return 127;
}
