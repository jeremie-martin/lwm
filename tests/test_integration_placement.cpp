#include "wm_observations.hpp"
#include <X11/keysym.h>
#include <catch2/catch_test_macros.hpp>

using namespace lwm::test;
namespace {
constexpr auto timeout = std::chrono::seconds(2);
constexpr auto config = R"(
[workspaces]
count = 2
[[binds]]
key = "F5"
move_to_workspace = 1
[[binds]]
key = "F6"
toggle_float = true
)";
void command(std::string const& socket, std::string const& text)
{
    auto reply = send_ipc_command(socket, text);
    REQUIRE(reply);
    REQUIRE(reply->starts_with("ok"));
}
nlohmann::json query(std::string const& socket, std::string const& text)
{
    auto reply = send_ipc_command(socket, text);
    REQUIRE(reply);
    REQUIRE(reply->starts_with("ok "));
    return nlohmann::json::parse(reply->substr(3));
}
void park_pointer(X11Connection& conn)
{
    xcb_warp_pointer(conn.get(), XCB_NONE, conn.root(), 0, 0, 0, 0, 0, 0);
    xcb_flush(conn.get());
}
}

TEST_CASE(
    "Integration: relocation preserves source history and destination membership through both entry paths",
    "[integration][placement]"
)
{
    auto env = TestEnvironment::create(config);
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    park_pointer(conn);
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 4; ++i)
    {
        auto w = create_window(conn, 20, 20, 200, 150);
        map_window(conn, w);
        windows.push_back(w);
        REQUIRE(wait_for_active_window(conn, w, timeout));
    }
    auto a = windows[0], b = windows[1], c = windows[2], d = windows[3];
    for (auto w : { b, a, d }) command(*socket, "focus window=" + std::to_string(w));
    send_client_message(conn, a, intern_atom(conn.get(), "WM_CHANGE_STATE"), XCB_ICCCM_WM_STATE_ICONIC);
    observe_title_after_events(conn, d);
    auto desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    SECTION("Key binding") { REQUIRE(send_key(conn, XK_F5)); }
    SECTION("EWMH request") { send_client_message(conn, d, desktop, 1); }
    observe_title_after_events(conn, d);
    // a is more recent but iconic; c is last in layout order. History chooses b.
    REQUIRE(wait_for_active_window(conn, b, timeout));
    REQUIRE(require_property_cardinal(conn.get(), d, desktop) == 1);
    auto counts = [&]
    {
        auto workspaces = query(*socket, "workspace list").at("monitors").at(0).at("workspaces");
        return std::pair{ workspaces.at(0).at("window_count").get<int>(),
                          workspaces.at(1).at("window_count").get<int>() };
    };
    REQUIRE(counts() == std::pair{ 3, 1 });
    send_client_message(conn, d, desktop, 99);
    send_client_message(conn, d, desktop, 1);
    observe_title_after_events(conn, d);
    REQUIRE(require_property_cardinal(conn.get(), d, desktop) == 1);
    REQUIRE(counts() == std::pair{ 3, 1 });
    command(*socket, "workspace switch 1");
    REQUIRE(wait_for_active_window(conn, d, timeout));
    send_client_message(conn, d, desktop, 0);
    observe_title_after_events(conn, d);
    REQUIRE(wait_for_active_window(conn, XCB_NONE, timeout));
    command(*socket, "workspace switch 0");
    REQUIRE(wait_for_active_window(conn, d, timeout));
    REQUIRE(counts() == std::pair{ 4, 0 });
    destroy_window(conn, c);
    observe_title_after_events(conn, d);
    REQUIRE(wait_for_active_window(conn, d, timeout));
    destroy_window(conn, d);
    REQUIRE(wait_for_active_window(conn, b, timeout));
    send_client_message(conn, b, intern_atom(conn.get(), "WM_CHANGE_STATE"), XCB_ICCCM_WM_STATE_ICONIC);
    observe_title_after_events(conn, b);
    REQUIRE(wait_for_active_window(conn, XCB_NONE, timeout));
    REQUIRE(counts() == std::pair{ 2, 0 });
    destroy_window(conn, a);
    destroy_window(conn, b);
    REQUIRE(wait_for_condition([&] { return counts() == std::pair{ 0, 0 }; }, timeout));
}

TEST_CASE(
    "Integration: kind round trips restore tile slots only within their original workspace",
    "[integration][placement]"
)
{
    auto env = TestEnvironment::create(config);
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    park_pointer(conn);
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 3; ++i)
    {
        auto w = create_window(conn, 20, 20, 200, 150);
        map_window(conn, w);
        windows.push_back(w);
        REQUIRE(wait_for_active_window(conn, w, timeout));
    }
    auto a = windows[0], b = windows[1], c = windows[2];
    std::vector<WindowGeometry> initial;
    for (auto w : windows) initial.push_back(require_window_geometry(conn, w));
    command(*socket, "focus window=" + std::to_string(b));
    auto toggle = [&]
    {
        REQUIRE(send_key(conn, XK_F6));
        observe_title_after_events(conn, b);
    };
    toggle();
    auto floating = require_window_geometry(conn, b);
    toggle();
    for (size_t i = 0; i < windows.size(); ++i) REQUIRE(require_window_geometry(conn, windows[i]) == initial[i]);
    toggle();
    REQUIRE(require_window_geometry(conn, b) == floating);
    auto desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    send_client_message(conn, b, desktop, 1);
    observe_title_after_events(conn, b);
    command(*socket, "workspace switch 1");
    REQUIRE(wait_for_active_window(conn, b, timeout));
    REQUIRE(require_window_geometry(conn, b) == floating);
    toggle();
    send_client_message(conn, b, desktop, 0);
    observe_title_after_events(conn, b);
    command(*socket, "workspace switch 0");
    // Relocation appends b after c; its saved slot from another workspace cannot reorder peers.
    REQUIRE(require_window_geometry(conn, c).x == require_window_geometry(conn, b).x);
    REQUIRE(require_window_geometry(conn, c).y < require_window_geometry(conn, b).y);
    REQUIRE(require_window_geometry(conn, a).x < require_window_geometry(conn, b).x);
    for (auto w : windows) destroy_window(conn, w);
}

TEST_CASE(
    "Integration: classification changes preserve hidden scratchpad membership",
    "[integration][placement][scratchpad]"
)
{
    auto env = TestEnvironment::create(config);
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    park_pointer(conn);
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto w = create_window(conn, 20, 20, 200, 150);
    REQUIRE(set_window_type(conn, w, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG")));
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, timeout));
    auto saved = require_window_geometry(conn, w);
    command(*socket, "scratchpad stash");
    REQUIRE(set_window_type(conn, w, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_NORMAL")));
    observe_title_after_events(conn, w);
    auto classification = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    REQUIRE(get_window_property_string(conn.get(), w, classification) == "floating");
    command(*socket, "workspace switch 1");
    command(*socket, "scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, w, timeout));
    auto workspaces = query(*socket, "workspace list").at("monitors").at(0).at("workspaces");
    REQUIRE(workspaces.at(0).at("window_count") == 0);
    REQUIRE(workspaces.at(1).at("window_count") == 0);
    REQUIRE(get_window_property_string(conn.get(), w, classification) == "floating");
    REQUIRE(require_window_geometry(conn, w) == saved);
    REQUIRE(require_property_cardinal(conn.get(), w, intern_atom(conn.get(), "_NET_WM_DESKTOP")) == 1);
    destroy_window(conn, w);
}
