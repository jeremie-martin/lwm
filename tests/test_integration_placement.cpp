#include "restart_handoff.hpp"
#include <X11/keysym.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
void park_pointer(X11Connection& conn)
{
    xcb_warp_pointer(conn.get(), XCB_NONE, conn.root(), 0, 0, 0, 0, 0, 0);
    xcb_flush(conn.get());
}
}

TEST_CASE(
    "Integration: relocation preserves source history and destination membership through both entry paths",
    "[integration][placement][focus][restart]"
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
    for (auto w : { b, a, d }) ipc_ok(*socket, "focus window=" + std::to_string(w));
    // The same subsequent operations must choose the same focus after exec.
    if (GENERATE(false, true))
    {
        auto previous = wm_instance(conn);
        REQUIRE(previous);
        ipc_ok(*socket, "restart");
        REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    }
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
        auto workspaces = ipc_json(*socket, "workspace list").at("monitors").at(0).at("workspaces");
        return std::pair{ workspaces.at(0).at("window_count").get<int>(),
                          workspaces.at(1).at("window_count").get<int>() };
    };
    REQUIRE(counts() == std::pair{ 3, 1 });
    send_client_message(conn, d, desktop, 99);
    send_client_message(conn, d, desktop, 1);
    observe_title_after_events(conn, d);
    REQUIRE(require_property_cardinal(conn.get(), d, desktop) == 1);
    REQUIRE(counts() == std::pair{ 3, 1 });
    ipc_ok(*socket, "workspace switch 1");
    REQUIRE(wait_for_active_window(conn, d, timeout));
    send_client_message(conn, d, desktop, 0);
    observe_title_after_events(conn, d);
    REQUIRE(wait_for_active_window(conn, XCB_NONE, timeout));
    ipc_ok(*socket, "workspace switch 0");
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
    ipc_ok(*socket, "focus window=" + std::to_string(b));
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
    ipc_ok(*socket, "workspace switch 1");
    REQUIRE(wait_for_active_window(conn, b, timeout));
    REQUIRE(require_window_geometry(conn, b) == floating);
    toggle();
    send_client_message(conn, b, desktop, 0);
    observe_title_after_events(conn, b);
    ipc_ok(*socket, "workspace switch 0");
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
    ipc_ok(*socket, "scratchpad stash");
    REQUIRE(set_window_type(conn, w, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_NORMAL")));
    observe_title_after_events(conn, w);
    auto classification = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    REQUIRE(get_window_property_string(conn.get(), w, classification) == "floating");
    ipc_ok(*socket, "workspace switch 1");
    ipc_ok(*socket, "scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, w, timeout));
    auto workspaces = ipc_json(*socket, "workspace list").at("monitors").at(0).at("workspaces");
    REQUIRE(workspaces.at(0).at("window_count") == 0);
    REQUIRE(workspaces.at(1).at("window_count") == 0);
    REQUIRE(get_window_property_string(conn.get(), w, classification) == "floating");
    REQUIRE(require_window_geometry(conn, w) == saved);
    REQUIRE(require_property_cardinal(conn.get(), w, intern_atom(conn.get(), "_NET_WM_DESKTOP")) == 1);
    destroy_window(conn, w);
}

TEST_CASE("Integration: restart preserves a floating tile's return position", "[integration][placement][restart][tile-slot]")
{
    bool restart = GENERATE(false, true);
    CAPTURE(restart);
    auto env = TestEnvironment::create(config);
    REQUIRE(env);
    auto& conn = env->conn;
    park_pointer(conn);
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 3; ++i)
    {
        auto window = create_window(conn, 20, 20, 200, 150);
        map_window(conn, window);
        REQUIRE(wait_for_active_window(conn, window, timeout));
        windows.push_back(window);
    }
    ipc_ok(*socket, "focus window=" + std::to_string(windows[1]));
    std::vector<WindowGeometry> before;
    for (auto window : windows) before.push_back(require_window_geometry(conn, window));
    ipc_ok(*socket, "window float");
    if (restart)
    {
        auto previous = wm_instance(conn);
        REQUIRE(previous);
        ipc_ok(*socket, "restart");
        REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
        socket = wait_for_ipc_socket_path(conn);
        REQUIRE(socket);
    }
    ipc_ok(*socket, "focus window=" + std::to_string(windows[1]));
    ipc_ok(*socket, "window float");
    for (size_t i = 0; i < windows.size(); ++i)
        CHECK(require_window_geometry(conn, windows[i]) == before[i]);
    for (auto window : windows) destroy_window(conn, window);
}

TEST_CASE(
    "Integration: restart preserves registration independently of stacking and tile order",
    "[integration][restart][registration]"
)
{
    auto restart_kind = GENERATE("restart", "failed-exec", "handoff");
    CAPTURE(restart_kind);
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    park_pointer(conn);
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto list_atom = intern_atom(conn.get(), "_NET_CLIENT_LIST");
    auto stack_atom = intern_atom(conn.get(), "_NET_CLIENT_LIST_STACKING");
    auto list = [&](xcb_atom_t atom)
    {
        return get_window_property_windows(conn.get(), conn.root(), atom);
    };
    std::vector<xcb_window_t> expected;
    auto create = [&](char const* type)
    {
        auto window = create_window(conn, 20, 20, 200, 150);
        if (type)
            set_window_type(conn, window, intern_atom(conn.get(), type));
        map_window(conn, window);
        expected.push_back(window);
        REQUIRE(wait_for_condition([&] { return list(list_atom) == expected; }, timeout));
        return window;
    };
    auto first = create(nullptr);
    auto dock = create("_NET_WM_WINDOW_TYPE_DOCK");
    auto second = create(nullptr);
    create("_NET_WM_WINDOW_TYPE_DESKTOP");
    auto third = create(nullptr);
    create("_NET_WM_WINDOW_TYPE_DIALOG");
    ipc_ok(*socket, "focus window=" + std::to_string(first));
    auto stacking = list(stack_atom);
    REQUIRE(stacking != expected);
    std::vector<WindowGeometry> tiles;
    for (auto window : { first, second, third }) tiles.push_back(require_window_geometry(conn, window));

    for (int iteration = 0; iteration < 2; ++iteration)
    {
        CAPTURE(iteration);
        auto previous = wm_instance(conn);
        REQUIRE(previous);
        if (iteration == 0 && std::string_view(restart_kind) == "handoff")
        {
            PausedRestart restart(env->wm, *socket);
            destroy_window(conn, dock);
            std::erase(expected, dock);
            // A new fixture is admitted before saved clients. Both new windows
            // must follow every survivor despite that admission order.
            for (auto type : { "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DIALOG" })
            {
                auto window = create_window(conn, 30, 30, 180, 120);
                set_window_type(conn, window, intern_atom(conn.get(), type));
                map_window(conn, window);
                expected.push_back(window);
                REQUIRE(get_window_geometry(conn, window));
            }
            restart.resume();
        }
        else
            ipc_ok(*socket, std::string_view(restart_kind) == "failed-exec" ? "exec /definitely/missing/lwm" : "restart");
        REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
        socket = wait_for_ipc_socket_path(conn);
        REQUIRE(socket);
        CHECK(list(list_atom) == expected);
        if (std::string_view(restart_kind) != "handoff")
            CHECK(list(stack_atom) == stacking);
        size_t index = 0;
        for (auto window : { first, second, third }) CHECK(require_window_geometry(conn, window) == tiles[index++]);
    }
    // Subsequent ordinary admission must follow the restored sequence too.
    create(nullptr);
    for (auto window : expected) destroy_window(conn, window);
}

TEST_CASE("Integration: startup places transient chains against the complete tiled scene", "[integration][placement][adoption]")
{
    bool child_first = GENERATE(false, true);
    CAPTURE(child_first);
    auto& server = X11TestEnvironment::instance();
    if (!server.available())
    {
        REQUIRE(std::getenv("LWM_TEST_REQUIRE_X11") == nullptr);
        SKIP("X11 unavailable");
    }
    X11Connection conn;
    REQUIRE(conn.ok());
    park_pointer(conn);
    auto create_parent = [&] { return create_window(conn, 40, 50, 200, 150); };
    auto create_dialog = [&] { return create_window(conn, 0, 0, 100, 80); };
    // The grandchild is scanned first, and the other tile last.
    auto grandchild = create_window(conn, 0, 0, 40, 20);
    auto first = child_first ? create_dialog() : create_parent();
    auto second = child_first ? create_parent() : create_dialog();
    auto parent = child_first ? second : first;
    auto dialog = child_first ? first : second;
    auto peer = create_window(conn, 20, 20, 200, 150);
    set_transient_for(conn, dialog, parent);
    set_transient_for(conn, grandchild, dialog);
    for (auto window : { grandchild, first, second, peer }) map_window(conn, window);
    REQUIRE(get_window_geometry(conn, peer));
    LwmProcess wm(server.display(), "[appearance]\npadding = 0\nborder_width = 0\n");
    REQUIRE(wait_for_wm_ready(conn, timeout));
    REQUIRE(wait_for_condition([&]
    {
        auto p = get_window_geometry(conn, parent);
        auto d = get_window_geometry(conn, dialog);
        auto g = get_window_geometry(conn, grandchild);
        return p && d && g && p->width == conn.screen()->width_in_pixels / 2
            && d->x == p->x + (p->width - d->width) / 2 && d->y == p->y + (p->height - d->height) / 2
            && g->x == d->x + (d->width - g->width) / 2 && g->y == d->y + (d->height - g->height) / 2;
    }, timeout));
    for (auto window : { grandchild, dialog, parent, peer }) destroy_window(conn, window);
}
