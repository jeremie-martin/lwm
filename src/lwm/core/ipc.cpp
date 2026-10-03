#include "ipc.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace lwm::ipc {

std::filesystem::path default_socket_path()
{
    char const* runtime = std::getenv("XDG_RUNTIME_DIR");
    auto directory = runtime ? std::filesystem::path(runtime) / "lwm" : std::filesystem::path("/tmp") / ("lwm-" + std::to_string(getuid()));
    char const* display = std::getenv("DISPLAY");
    std::string name = display && *display ? display : "default";
    std::ranges::replace_if(name, [](unsigned char ch) { return !std::isalnum(ch) && ch != '.' && ch != '_' && ch != '-'; }, '_');
    return directory / ("ipc-" + name + ".sock");
}

} // namespace lwm::ipc
