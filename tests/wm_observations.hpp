#pragma once
#include "x11_test_harness.hpp"
#include <nlohmann/json.hpp>

namespace lwm::test {
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
