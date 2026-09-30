#pragma once
#include "x11_test_harness.hpp"
#include <nlohmann/json.hpp>

namespace lwm::test {
// Sends an IPC command that must succeed and returns its complete reply.
inline std::string ipc_ok(std::string const& socket, std::string const& command)
{
    auto reply = send_ipc_command(socket, command);
    REQUIRE(reply);
    INFO(command << " -> " << *reply);
    REQUIRE(reply->starts_with("ok"));
    return *reply;
}

// Sends a query that must succeed and returns its JSON value.
inline nlohmann::json ipc_json(std::string const& socket, std::string const& command)
{
    auto reply = ipc_ok(socket, command);
    REQUIRE(reply.starts_with("ok "));
    return nlohmann::json::parse(reply.substr(3));
}

// X resource IDs can be reused immediately after disconnect. IPC's instance
// identifies a WM lifetime even when its supporting-window ID is reused.
inline std::optional<std::string> wm_instance(X11Connection& conn)
{
    auto atom = intern_atom(conn.get(), "_LWM_IPC_SOCKET");
    auto path = get_window_property_string(conn.get(), conn.root(), atom);
    if (!path || path->empty())
        return std::nullopt;
    auto reply = send_ipc_command(*path, "state");
    if (!reply || !reply->starts_with("ok "))
        return std::nullopt;
    return nlohmann::json::parse(reply->substr(3)).at("instance").get<std::string>();
}

inline bool wait_for_wm_restart(X11Connection& conn, std::chrono::milliseconds timeout, std::string const& previous)
{
    return wait_for_condition(
        [&]
        {
            auto current = wm_instance(conn);
            return current && *current != previous;
        },
        timeout
    );
}

// Use only on a managed window whose title does not participate in rules.
// The WM observes this title after earlier events sent on this X connection.
// Reading it through IPC proves handling/completion, unlike an X client roundtrip.
inline void observe_title_after_events(X11Connection& conn, xcb_window_t window)
{
    static uint64_t sequence = 0;
    auto title = "test-event-marker-" + std::to_string(++sequence);
    auto name = intern_atom(conn.get(), "_NET_WM_NAME");
    auto utf8 = intern_atom(conn.get(), "UTF8_STRING");
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, window, name, utf8, 8, title.size(), title.data());
    xcb_flush(conn.get());
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    REQUIRE(wait_for_condition(
        [&]
        {
            auto reply = send_ipc_command(*socket, "window list");
            if (!reply)
                return false;
            REQUIRE(reply->starts_with("ok "));
            auto snapshot = nlohmann::json::parse(reply->substr(3));
            for (auto const& entry : snapshot.at("windows"))
                if (entry.at("id") == window && entry.at("title") == title)
                    return true;
            return false;
        },
        std::chrono::seconds(2)
    ));
}
} // namespace lwm::test
