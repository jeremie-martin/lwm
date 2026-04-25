#include "lwm/core/ipc.hpp"
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#include <xcb/xcb.h>

namespace {

std::string trim_ascii(std::string_view value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
        ++start;

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;

    return std::string(value.substr(start, end - start));
}

void print_usage()
{
    std::cerr << "usage: lwmctl [--socket PATH] <command>\n"
              << "\n"
              << "commands:\n"
              << "  ping                     check if WM is running\n"
              << "  version                  show WM version\n"
              << "  reload-config            reload configuration\n"
              << "  restart                  restart WM\n"
              << "  exec PATH               restart WM with a different binary\n"
              << "  layout set NAME          set layout strategy (master-stack)\n"
              << "  ratio set VALUE          set master split ratio (0.1-0.9)\n"
              << "  ratio reset              reset all split ratios to defaults\n"
              << "  ratio adjust DELTA       adjust master ratio by delta (e.g. +0.05)\n"
              << "  notify-attention window=<xid>\n"
              << "                           mark a managed window as needing attention\n"
              << "  subscribe [FILTER]       stream events as JSON lines\n"
              << "                           filter: comma-separated event types\n"
              << "                           (window_map,window_unmap,focus_change,\n"
              << "                            workspace_switch,layout_change,config_reload,\n"
              << "                            key_action)\n";
}

std::optional<std::string> root_socket_path()
{
    int screen_index = 0;
    xcb_connection_t* conn = xcb_connect(nullptr, &screen_index);
    if (!conn || xcb_connection_has_error(conn))
    {
        if (conn)
            xcb_disconnect(conn);
        return std::nullopt;
    }

    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; iter.rem && i < screen_index; ++i)
        xcb_screen_next(&iter);
    if (!iter.rem)
    {
        xcb_disconnect(conn);
        return std::nullopt;
    }

    auto value = lwm::ipc::get_root_text_property(conn, iter.data->root, "_LWM_IPC_SOCKET");
    xcb_disconnect(conn);
    return value;
}

std::string resolve_socket_path(std::optional<std::string> const& cli_socket)
{
    if (cli_socket.has_value())
        return *cli_socket;

    if (char const* env_socket = std::getenv("LWM_SOCKET"))
        return env_socket;

    if (auto root_socket = root_socket_path())
        return *root_socket;

    return lwm::ipc::default_socket_path().string();
}

int connect_socket(std::string const& socket_path)
{
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path))
    {
        std::cerr << "socket path too long: " << socket_path << '\n';
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        std::cerr << "failed to create socket: " << std::strerror(errno) << '\n';
        return -1;
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::cerr << "failed to connect to " << socket_path << ": " << std::strerror(errno) << '\n';
        close(fd);
        return -1;
    }

    return fd;
}

int run_command(std::string const& socket_path, std::string const& command)
{
    int fd = connect_socket(socket_path);
    if (fd < 0)
        return 1;

    std::string request = command;
    request.push_back('\n');

    if (send(fd, request.data(), request.size(), 0) < 0)
    {
        std::cerr << "failed to send request: " << std::strerror(errno) << '\n';
        close(fd);
        return 1;
    }

    shutdown(fd, SHUT_WR);

    std::string response;
    std::vector<char> buffer(1024);
    while (true)
    {
        ssize_t bytes_read = recv(fd, buffer.data(), buffer.size(), 0);
        if (bytes_read < 0)
        {
            std::cerr << "failed to read response: " << std::strerror(errno) << '\n';
            close(fd);
            return 1;
        }
        if (bytes_read == 0)
            break;
        response.append(buffer.data(), static_cast<size_t>(bytes_read));
    }

    close(fd);

    response = trim_ascii(response);
    if (response == "ok")
        return 0;
    if (response.rfind("ok ", 0) == 0)
    {
        std::cout << response.substr(3) << '\n';
        return 0;
    }
    if (response == "error")
        return 1;
    if (response.rfind("error ", 0) == 0)
    {
        std::cerr << response.substr(6) << '\n';
        return 1;
    }

    if (!response.empty())
        std::cout << response << '\n';
    return 0;
}

int run_subscribe(std::string const& socket_path, std::string const& filter)
{
    int fd = connect_socket(socket_path);
    if (fd < 0)
        return 1;

    std::string request = "subscribe";
    if (!filter.empty())
    {
        request.push_back(' ');
        request.append(filter);
    }
    request.push_back('\n');

    if (send(fd, request.data(), request.size(), 0) < 0)
    {
        std::cerr << "failed to send subscribe request: " << std::strerror(errno) << '\n';
        close(fd);
        return 1;
    }

    // Read initial response (ok subscribed or error ...)
    std::string initial;
    std::vector<char> buf(4096);
    while (true)
    {
        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0)
        {
            std::cerr << "connection closed before subscription confirmed\n";
            close(fd);
            return 1;
        }
        initial.append(buf.data(), static_cast<size_t>(n));
        if (initial.find('\n') != std::string::npos)
            break;
    }

    // Check for error response
    auto first_line_end = initial.find('\n');
    std::string first_line = initial.substr(0, first_line_end);
    if (first_line.rfind("error", 0) == 0)
    {
        std::cerr << (first_line.size() > 6 ? first_line.substr(6) : "subscription failed") << '\n';
        close(fd);
        return 1;
    }

    // Ignore SIGPIPE so a closed stdout consumer becomes a normal write failure.
    signal(SIGPIPE, SIG_IGN);

    auto write_stdout = [](char const* data, size_t size) -> bool
    {
        std::cout.write(data, static_cast<std::streamsize>(size));
        std::cout.flush();
        return static_cast<bool>(std::cout);
    };

    // Print any remaining data after the first line (unlikely but possible)
    if (first_line_end + 1 < initial.size()
        && !write_stdout(initial.data() + first_line_end + 1, initial.size() - first_line_end - 1))
    {
        close(fd);
        return 0;
    }

    // Stream events until EOF or SIGINT
    while (true)
    {
        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0)
            break;
        if (!write_stdout(buf.data(), static_cast<size_t>(n)))
            break;
    }

    close(fd);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    std::optional<std::string> cli_socket;
    std::vector<std::string> args;
    args.reserve(argc > 0 ? static_cast<size_t>(argc - 1) : 0);

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--socket")
        {
            if (i + 1 >= argc)
            {
                print_usage();
                return 1;
            }
            cli_socket = argv[++i];
            continue;
        }
        if (arg == "--help" || arg == "-h")
        {
            print_usage();
            return 0;
        }
        args.push_back(std::move(arg));
    }

    if (args.empty())
    {
        print_usage();
        return 1;
    }

    std::string const& command = args[0];
    std::string socket_path = resolve_socket_path(cli_socket);

    // Commands with arguments
    if (command == "exec")
    {
        if (args.size() != 2)
        {
            std::cerr << "usage: lwmctl exec PATH\n";
            return 1;
        }
        return run_command(socket_path, "exec " + args[1]);
    }

    if (command == "layout")
    {
        if (args.size() != 3 || args[1] != "set")
        {
            std::cerr << "usage: lwmctl layout set NAME\n";
            return 1;
        }
        return run_command(socket_path, "layout set " + args[2]);
    }

    if (command == "ratio")
    {
        if (args.size() < 2)
        {
            std::cerr << "usage: lwmctl ratio <set VALUE|reset|adjust DELTA>\n";
            return 1;
        }
        if (args[1] == "reset" && args.size() == 2)
            return run_command(socket_path, "ratio reset");
        if (args[1] == "set" && args.size() == 3)
            return run_command(socket_path, "ratio set " + args[2]);
        if (args[1] == "adjust" && args.size() == 3)
            return run_command(socket_path, "ratio adjust " + args[2]);
        std::cerr << "usage: lwmctl ratio <set VALUE|reset|adjust DELTA>\n";
        return 1;
    }

    if (command == "notify-attention")
    {
        if (args.size() != 2)
        {
            std::cerr << "usage: lwmctl notify-attention window=<xid>\n";
            return 1;
        }
        std::string ipc_cmd = "notify-attention " + args[1];
        return run_command(socket_path, ipc_cmd);
    }

    if (command == "subscribe")
    {
        std::string filter;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (!filter.empty())
                filter.push_back(',');
            filter.append(args[i]);
        }
        return run_subscribe(socket_path, filter);
    }

    // Simple commands (no arguments)
    if (args.size() != 1)
    {
        print_usage();
        return 1;
    }

    if (command != "ping" && command != "version" && command != "reload-config" && command != "restart")
    {
        print_usage();
        return 1;
    }

    return run_command(socket_path, command);
}
