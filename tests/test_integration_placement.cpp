#include "restart_handoff.hpp"
#include <X11/keysym.h>
#include <xcb/xcb_icccm.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace lwm::test;
namespace {
constexpr auto timeout = std::chrono::seconds(2);
constexpr auto config = R"(
[workspaces]
names = ["1", "2"]
[binds]
"F5" = "window to-workspace 1"
"F6" = "window float"
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
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 4; ++i)
    {
        auto w = create_window(conn, 20, 20, 200, 150);
        map_window(conn, w);
        windows.push_back(w);
        REQUIRE(wait_for_active_window(conn, w, timeout));
    }
    auto a = windows[0], b = windows[1], c = windows[2], d = windows[3];
    for (auto w : { b, a, d }) ipc_ok("focus window=" + std::to_string(w));
    // The same subsequent operations must choose the same focus after exec.
    if (GENERATE(false, true))
    {
        auto previous = wm_instance(conn);
        REQUIRE(previous);
        ipc_ok("restart");
        REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    }
    stash(a);
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
        auto workspaces = ipc_json("workspace list").at("monitors").at(0).at("workspaces");
        return std::pair{ workspaces.at(0).at("window_count").get<int>(),
                          workspaces.at(1).at("window_count").get<int>() };
    };
    REQUIRE(counts() == std::pair{ 3, 1 });
    send_client_message(conn, d, desktop, 99);
    send_client_message(conn, d, desktop, 1);
    observe_title_after_events(conn, d);
    REQUIRE(require_property_cardinal(conn.get(), d, desktop) == 1);
    REQUIRE(counts() == std::pair{ 3, 1 });
    ipc_ok("workspace switch 1");
    REQUIRE(wait_for_active_window(conn, d, timeout));
    send_client_message(conn, d, desktop, 0);
    observe_title_after_events(conn, d);
    REQUIRE(wait_for_active_window(conn, XCB_NONE, timeout));
    ipc_ok("workspace switch 0");
    REQUIRE(wait_for_active_window(conn, d, timeout));
    REQUIRE(counts() == std::pair{ 4, 0 });
    destroy_window(conn, c);
    observe_title_after_events(conn, d);
    REQUIRE(wait_for_active_window(conn, d, timeout));
    destroy_window(conn, d);
    REQUIRE(wait_for_active_window(conn, b, timeout));
    stash(b);
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
    ipc_ok("focus window=" + std::to_string(b));
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
    ipc_ok("workspace switch 1");
    REQUIRE(wait_for_active_window(conn, b, timeout));
    REQUIRE(require_window_geometry(conn, b) == floating);
    toggle();
    send_client_message(conn, b, desktop, 0);
    observe_title_after_events(conn, b);
    ipc_ok("workspace switch 0");
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
    auto w = create_window(conn, 20, 20, 200, 150);
    REQUIRE(set_window_type(conn, w, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG")));
    map_window(conn, w);
    REQUIRE(wait_for_active_window(conn, w, timeout));
    auto saved = require_window_geometry(conn, w);
    ipc_ok("scratchpad stash");
    REQUIRE(set_window_type(conn, w, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_NORMAL")));
    observe_title_after_events(conn, w);
    auto classification = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    REQUIRE(get_window_property_string(conn.get(), w, classification) == "floating");
    ipc_ok("workspace switch 1");
    ipc_ok("scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, w, timeout));
    auto workspaces = ipc_json("workspace list").at("monitors").at(0).at("workspaces");
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
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 3; ++i)
    {
        auto window = create_window(conn, 20, 20, 200, 150);
        map_window(conn, window);
        REQUIRE(wait_for_active_window(conn, window, timeout));
        windows.push_back(window);
    }
    ipc_ok("focus window=" + std::to_string(windows[1]));
    std::vector<WindowGeometry> before;
    for (auto window : windows) before.push_back(require_window_geometry(conn, window));
    ipc_ok("window float");
    if (restart)
    {
        auto previous = wm_instance(conn);
        REQUIRE(previous);
        ipc_ok("restart");
        REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    }
    ipc_ok("focus window=" + std::to_string(windows[1]));
    ipc_ok("window float");
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
    ipc_ok("focus window=" + std::to_string(first));
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
            PausedRestart restart(env->wm);
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
            ipc_ok(std::string_view(restart_kind) == "failed-exec" ? "exec /definitely/missing/lwm" : "restart");
        REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
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

TEST_CASE(
    "Integration: restart preserves representation while observing classification and restoring pool ownership",
    "[integration][restart][placement][property][scratchpad]"
)
{
    auto env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\"]\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    park_pointer(conn);
    auto parent = create_window(conn, 20, 20, 200, 150);
    auto pooled = create_window(conn, 40, 40, 200, 150);
    for (auto window : { parent, pooled })
    {
        map_window(conn, window);
        REQUIRE(wait_for_active_window(conn, window, timeout));
    }
    ipc_ok("scratchpad stash");
    REQUIRE(wait_for_active_window(conn, parent, timeout));
    REQUIRE(is_hidden_offscreen(conn, pooled));
    auto geometry = require_window_geometry(conn, parent);
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    PausedRestart restart(env->wm);
    auto utility = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_UTILITY");
    auto dialog = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    auto normal = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_NORMAL");
    auto states = intern_atom(conn.get(), "_NET_WM_STATE");
    auto skip = intern_atom(conn.get(), "_NET_WM_STATE_SKIP_TASKBAR");
    auto kind = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    REQUIRE(set_window_type(conn, parent, utility));
    REQUIRE(set_window_type(conn, pooled, utility));
    set_transient_for(conn, parent, pooled);
    auto child = create_window(conn, 0, 0, 120, 80);
    REQUIRE(set_window_type(conn, child, dialog));
    set_transient_for(conn, child, parent);
    map_window(conn, child);
    REQUIRE(get_window_geometry(conn, child)); // Complete the handoff edits on X.
    restart.resume();
    REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    REQUIRE(wait_for_condition(
        [&] { return get_window_property_string(conn.get(), parent, kind) == "tiled"
                   && get_window_property_string(conn.get(), child, kind) == "floating"; },
        timeout
    ));
    CHECK(require_window_geometry(conn, parent) == geometry);
    CHECK(property_has_atom(conn.get(), parent, states, skip));
    CHECK(get_window_property_string(conn.get(), pooled, kind) == "tiled");
    CHECK(is_hidden_offscreen(conn, pooled));
    CHECK(ipc_json("scratchpad list").at("pool") == nlohmann::json::array({ pooled }));
    CHECK(wait_for_active_window(conn, parent, timeout));
    auto child_geometry = require_window_geometry(conn, child);
    CHECK(child_geometry.x == geometry.x + (geometry.width - child_geometry.width) / 2);
    CHECK(child_geometry.y == geometry.y + (geometry.height - child_geometry.height) / 2);

    // The observed type drives layer/skip defaults immediately, but saved mode
    // is private intent. Later metadata changes still drive ordinary mode defaults.
    REQUIRE(set_window_type(conn, parent, dialog));
    observe_title_after_events(conn, parent);
    CHECK(get_window_property_string(conn.get(), parent, kind) == "floating");
    xcb_delete_property(conn.get(), parent, intern_atom(conn.get(), "WM_TRANSIENT_FOR"));
    REQUIRE(set_window_type(conn, parent, normal));
    observe_title_after_events(conn, parent);
    CHECK(get_window_property_string(conn.get(), parent, kind) == "tiled");
    ipc_ok("scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, pooled, timeout));
    CHECK(get_window_property_string(conn.get(), pooled, kind) == "tiled");
    CHECK_FALSE(is_hidden_offscreen(conn, pooled));
    CHECK(ipc_json("scratchpad list").at("pool") == nlohmann::json::array({ pooled }));
    for (auto window : { child, pooled, parent }) destroy_window(conn, window);
}

TEST_CASE("Integration: restart preserves client ownership across non-client type hints",
          "[integration][restart][placement][property][scratchpad][fullscreen]")
{
    auto type = GENERATE("DOCK", "DESKTOP", "POPUP_MENU");
    auto path = GENERATE("restart", "failed-exec", "handoff");
    CAPTURE(type, path);
    auto env = TestEnvironment::create(R"(
[workspaces]
names = ["1", "2"]
[[scratchpads]]
name = "named"
spawn = ["/bin/true"]
match = { class = "NamedOwner" }
)");
    REQUIRE(env);
    auto& conn = env->conn;
    park_pointer(conn);
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 4; ++i)
    {
        auto window = create_window(conn, 20, 30, 160, 100);
        if (i == 3) set_window_wm_class(conn, window, "named", "NamedOwner");
        map_window(conn, window);
        windows.push_back(window);
        REQUIRE(wait_for_condition([&] { return ipc_json("window list")["windows"].size() == windows.size(); }, timeout));
    }
    auto tile = windows[0], remote = windows[1], pooled = windows[2], named = windows[3];
    ipc_ok("focus window=" + std::to_string(remote));
    ipc_ok("window to-workspace 1");
    ipc_ok("focus window=" + std::to_string(pooled));
    ipc_ok("window float");
    ipc_ok("scratchpad stash");
    ipc_ok("focus window=" + std::to_string(tile));
    ipc_ok("window fullscreen");
    // A client that advertises dock struts still has no dock ownership.
    auto strut_atom = intern_atom(conn.get(), "_NET_WM_STRUT");
    uint32_t strut[] = { 0, 0, 160, 0 };
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, tile, strut_atom, XCB_ATOM_CARDINAL, 32, 4, strut);
    observe_title_after_events(conn, tile);
    auto workarea_atom = intern_atom(conn.get(), "_NET_WORKAREA");
    auto workarea = read_property32(conn.get(), conn.root(), workarea_atom, XCB_ATOM_CARDINAL);
    REQUIRE(workarea);
    auto before = ipc_json("state");
    auto geometry = require_window_geometry(conn, tile);
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    auto changed_type = intern_atom(conn.get(), (std::string("_NET_WM_WINDOW_TYPE_") + type).c_str());
    auto change = [&]
    {
        for (auto window : windows) REQUIRE(set_window_type(conn, window, changed_type));
    };
    if (std::string_view(path) == "handoff")
    {
        PausedRestart restart(env->wm);
        change();
        REQUIRE(get_window_geometry(conn, tile));
        restart.resume();
    }
    else
    {
        change();
        observe_title_after_events(conn, tile);
        ipc_ok(std::string_view(path) == "failed-exec" ? "exec /definitely/missing/lwm" : "restart");
    }
    REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    auto after = ipc_json("state");
    CHECK(after["scratchpads"] == before["scratchpads"]);
    CHECK(after["workspaces"] == before["workspaces"]);
    REQUIRE(after["windows"]["windows"].size() == windows.size());
    for (size_t i = 0; i < windows.size(); ++i)
        for (auto field : { "id", "kind", "monitor", "workspace", "iconic", "fullscreen" })
            CHECK(after["windows"]["windows"][i][field] == before["windows"]["windows"][i][field]);
    CHECK(wait_for_active_window(conn, tile, timeout));
    CHECK(require_window_geometry(conn, tile) == geometry);
    CHECK(read_property32(conn.get(), conn.root(), workarea_atom, XCB_ATOM_CARDINAL) == workarea);
    CHECK(is_hidden_offscreen(conn, pooled));
    CHECK(is_hidden_offscreen(conn, named));
    auto list_atom = intern_atom(conn.get(), "_NET_CLIENT_LIST");
    CHECK(get_window_property_windows(conn.get(), conn.root(), list_atom) == windows);

    // Identical hints still choose fresh admission roles for a newcomer.
    auto fresh = create_window(conn, 0, 0, 80, 40);
    REQUIRE(set_window_type(conn, fresh, changed_type));
    map_window(conn, fresh);
    observe_title_after_events(conn, tile);
    CHECK(ipc_json("window list")["windows"].size() == windows.size());
    if (std::string_view(type) != "POPUP_MENU") windows.push_back(fresh);
    CHECK(get_window_property_windows(conn.get(), conn.root(), list_atom) == windows);
    destroy_window(conn, fresh);
    for (auto window : { tile, remote, pooled, named }) destroy_window(conn, window);
}

TEST_CASE("Integration: restart preserves fixture roles across changed admission hints",
          "[integration][restart][property][fixture]")
{
    auto role = GENERATE("DOCK", "DESKTOP");
    auto type = GENERATE("NORMAL", "DOCK", "DESKTOP", "POPUP_MENU");
    auto path = GENERATE("restart", "failed-exec", "handoff");
    CAPTURE(role, type, path);
    auto env = TestEnvironment::create();
    REQUIRE(env);
    auto& conn = env->conn;
    park_pointer(conn);
    auto fixture = create_window(conn, 0, 0, 100, 30);
    REQUIRE(set_window_type(conn, fixture, intern_atom(conn.get(), (std::string("_NET_WM_WINDOW_TYPE_") + role).c_str())));
    uint32_t strut[] = { 0, 0, 40, 0 };
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, fixture, intern_atom(conn.get(), "_NET_WM_STRUT"),
                        XCB_ATOM_CARDINAL, 32, 4, strut);
    map_window(conn, fixture);
    auto kind = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    auto expected = std::string_view(role) == "DOCK" ? "dock" : "desktop";
    REQUIRE(wait_for_condition([&] { return get_window_property_string(conn.get(), fixture, kind) == expected; }, timeout));
    auto client = create_window(conn, 40, 40, 100, 80);
    map_window(conn, client);
    REQUIRE(wait_for_active_window(conn, client, timeout));
    auto workarea_atom = intern_atom(conn.get(), "_NET_WORKAREA");
    auto workarea = read_property32(conn.get(), conn.root(), workarea_atom, XCB_ATOM_CARDINAL);
    REQUIRE(workarea);
    auto geometry = require_window_geometry(conn, client);
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    auto change = [&] { REQUIRE(set_window_type(conn, fixture, intern_atom(conn.get(), (std::string("_NET_WM_WINDOW_TYPE_") + type).c_str()))); };
    if (std::string_view(path) == "handoff")
    {
        PausedRestart restart(env->wm);
        change();
        REQUIRE(get_window_geometry(conn, fixture));
        restart.resume();
    }
    else
    {
        change();
        observe_title_after_events(conn, client);
        ipc_ok(std::string_view(path) == "failed-exec" ? "exec /definitely/missing/lwm" : "restart");
    }
    REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    CHECK(get_window_property_string(conn.get(), fixture, kind) == expected);
    CHECK(ipc_json("window list")["windows"].size() == 1);
    CHECK(wait_for_active_window(conn, client, timeout));
    CHECK(read_property32(conn.get(), conn.root(), workarea_atom, XCB_ATOM_CARDINAL) == workarea);
    CHECK(require_window_geometry(conn, client) == geometry);
    CHECK(get_window_property_windows(conn.get(), conn.root(), intern_atom(conn.get(), "_NET_CLIENT_LIST"))
          == std::vector<xcb_window_t>{ fixture, client });
    destroy_window(conn, fixture);
    destroy_window(conn, client);
}

TEST_CASE("Integration: floating hints agree across live admission, startup and restart",
          "[integration][placement][adoption][wm_normal_hints][restart]")
{
    bool startup = GENERATE(false, true);
    bool anchored = GENERATE(false, true);
    auto position = GENERATE("absent", "accepted", "rejected");
    CAPTURE(startup, anchored, position);
    auto& server = X11TestEnvironment::instance();
    if (!server.available())
    {
        REQUIRE(std::getenv("LWM_TEST_REQUIRE_X11") == nullptr);
        SKIP("X11 unavailable");
    }
    X11Connection conn;
    REQUIRE(conn.ok());
    park_pointer(conn);
    std::unique_ptr<LwmProcess> wm;
    auto start = [&]
    {
        wm = std::make_unique<LwmProcess>(server.display(), "[appearance]\npadding = 0\nborder_width = 0\n");
        REQUIRE(wait_for_wm_ready(conn, timeout));
    };
    if (!startup)
        start();
    auto parent = create_window(conn, 0, 0, 200, 150);
    auto peer = create_window(conn, 0, 0, 200, 150);
    for (auto window : { parent, peer }) map_window(conn, window);
    if (!startup)
        REQUIRE(wait_for_active_window(conn, peer, timeout));
    auto dialog = create_window(conn, 0, 0, 150, 90);
    set_window_type(conn, dialog, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    if (anchored)
        set_transient_for(conn, dialog, parent);
    xcb_size_hints_t hints{};
    hints.flags = XCB_ICCCM_SIZE_HINT_US_SIZE;
    hints.width = 150;
    hints.height = 90;
    if (std::string_view(position) != "absent")
    {
        hints.flags |= XCB_ICCCM_SIZE_HINT_US_POSITION;
        hints.x = std::string_view(position) == "accepted" ? 120 : 2000;
        hints.y = 130;
    }
    xcb_icccm_set_wm_normal_hints(conn.get(), dialog, &hints);
    map_window(conn, dialog);
    if (startup)
        start();
    // Both paths must use the complete tiled scene when centering on a parent.
    REQUIRE(wait_for_condition([&]
    {
        auto rectangle = get_window_geometry(conn, parent);
        return rectangle && rectangle->width == conn.screen()->width_in_pixels / 2;
    }, timeout));
    auto area = anchored ? require_window_geometry(conn, parent)
                         : WindowGeometry{ 0, 0, conn.screen()->width_in_pixels, conn.screen()->height_in_pixels };
    int16_t x = std::string_view(position) == "accepted" ? 120 : area.x + (area.width - 150) / 2;
    int16_t y = std::string_view(position) == "accepted" ? 130 : area.y + (area.height - 90) / 2;
    REQUIRE(wait_for_window_geometry(conn, dialog, x, y, 150, 90));
    // Repeating positional hints is stable. A size-only update preserves the
    // chosen origin rather than replaying initial centering.
    xcb_icccm_set_wm_normal_hints(conn.get(), dialog, &hints);
    observe_title_after_events(conn, dialog);
    CHECK(require_window_geometry(conn, dialog) == WindowGeometry{ x, y, 150, 90 });
    hints.flags = XCB_ICCCM_SIZE_HINT_US_SIZE;
    hints.width = 170;
    hints.height = 110;
    xcb_icccm_set_wm_normal_hints(conn.get(), dialog, &hints);
    REQUIRE(wait_for_window_geometry(conn, dialog, x, y, 170, 110));
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    ipc_ok("restart");
    REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    CHECK(require_window_geometry(conn, dialog) == WindowGeometry{ x, y, 170, 110 });
    for (auto window : { dialog, peer, parent }) destroy_window(conn, window);
}

TEST_CASE("Integration: cold startup preserves fullscreen claim order across tiled rules and application state",
          "[integration][placement][adoption][rules][fullscreen][restart]")
{
    bool rule_first = GENERATE(false, true);
    bool floating_application = GENERATE(false, true);
    CAPTURE(rule_first, floating_application);
    auto& server = X11TestEnvironment::instance();
    if (!server.available())
    {
        REQUIRE(std::getenv("LWM_TEST_REQUIRE_X11") == nullptr);
        SKIP("X11 unavailable");
    }
    X11Connection conn;
    REQUIRE(conn.ok());
    park_pointer(conn);
    auto first = create_window(conn, 20, 30, 200, 150);
    auto second = create_window(conn, 40, 50, 220, 160);
    auto ruled = rule_first ? first : second;
    auto application = rule_first ? second : first;
    set_window_wm_class(conn, ruled, "test", "StartupFullscreen");
    if (floating_application)
        set_window_type(conn, application, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG"));
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    auto states = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, application, states, XCB_ATOM_ATOM, 32, 1, &fullscreen);
    for (auto window : { first, second }) map_window(conn, window);
    REQUIRE(get_window_geometry(conn, second));
    LwmProcess wm(server.display(), R"(
[appearance]
padding = 0
border_width = 0
[[rules]]
match = { class = "StartupFullscreen" }
apply = { fullscreen = true }
)");
    REQUIRE(wait_for_wm_ready(conn, timeout));
    REQUIRE(wait_for_active_window(conn, second, timeout));
    REQUIRE(wait_for_window_geometry(conn, second, 0, 0, conn.screen()->width_in_pixels, conn.screen()->height_in_pixels));
    REQUIRE(wait_for_condition([&]
    {
        auto geometry = get_window_geometry(conn, first);
        return geometry && geometry->x < 0;
    }, timeout));
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    ipc_ok("restart");
    REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    CHECK(wait_for_active_window(conn, second, timeout));
    for (auto window : { first, second }) destroy_window(conn, window);
}

TEST_CASE("Integration: adoption resolves mixed transient chains before publishing placement",
          "[integration][placement][adoption][restart]")
{
    bool handoff = GENERATE(false, true);
    CAPTURE(handoff);
    auto& server = X11TestEnvironment::instance();
    if (!server.available())
    {
        REQUIRE(std::getenv("LWM_TEST_REQUIRE_X11") == nullptr);
        SKIP("X11 unavailable");
    }
    X11Connection conn;
    REQUIRE(conn.ok());
    park_pointer(conn);
    constexpr auto configuration = R"(
[appearance]
padding = 0
border_width = 0
[workspaces]
names = ["1", "2"]
[[rules]]
match = { class = "StartupParent" }
apply = { workspace = 1 }
[[rules]]
match = { class = "StartupTile" }
apply = { floating = false }
)";
    std::unique_ptr<LwmProcess> wm;
    std::unique_ptr<PausedRestart> paused;
    std::optional<std::string> previous;
    if (handoff)
    {
        wm = std::make_unique<LwmProcess>(server.display(), configuration);
        REQUIRE(wait_for_wm_ready(conn, timeout));
        previous = wm_instance(conn);
        REQUIRE(previous);
        paused = std::make_unique<PausedRestart>(*wm);
    }
    // Creation order makes both descendants precede their parent in QueryTree.
    auto floating = create_window(conn, 0, 0, 180, 100);
    auto tile = create_window(conn, 0, 0, 200, 150);
    auto parent = create_window(conn, 0, 0, 200, 150);
    auto peer = create_window(conn, 0, 0, 200, 150);
    set_transient_for(conn, floating, parent);
    set_transient_for(conn, tile, floating);
    set_window_wm_class(conn, parent, "parent", "StartupParent");
    set_window_wm_class(conn, tile, "tile", "StartupTile");
    auto desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    uint32_t workspace = 1;
    xcb_change_property(conn.get(), XCB_PROP_MODE_REPLACE, peer, desktop, XCB_ATOM_CARDINAL, 32, 1, &workspace);
    for (auto window : { floating, tile, parent, peer }) map_window(conn, window);
    REQUIRE(get_window_geometry(conn, peer)); // Complete the pre-adoption scene.
    if (handoff)
    {
        paused->resume();
        REQUIRE(wait_for_wm_restart(conn, timeout, *previous));
    }
    else
    {
        wm = std::make_unique<LwmProcess>(server.display(), configuration);
        REQUIRE(wait_for_wm_ready(conn, timeout));
    }
    for (auto window : { floating, tile, parent, peer })
        REQUIRE(wait_for_condition([&] { return get_window_property_cardinal(conn.get(), window, desktop) == 1; }, timeout));
    ipc_ok("workspace switch 1");
    auto windows = ipc_json("window list").at("windows");
    REQUIRE(windows.size() == 4);
    REQUIRE(wait_for_condition([&]
    {
        auto outer = get_window_geometry(conn, parent);
        auto inner = get_window_geometry(conn, floating);
        return outer && inner && outer->x + outer->width / 2 == inner->x + inner->width / 2
            && outer->y + outer->height / 2 == inner->y + inner->height / 2;
    }, timeout));
    auto counts = ipc_json("workspace list").at("monitors").at(0).at("workspaces");
    CHECK(counts.at(0).at("window_count") == 0);
    CHECK(counts.at(1).at("window_count") == 3);
    for (auto window : { floating, tile, parent, peer }) destroy_window(conn, window);
}

TEST_CASE("Integration: border reloads and partial geometry requests preserve frame boundaries", "[integration][geometry][border]")
{
    auto& server = X11TestEnvironment::instance();
    if (!server.available()) SKIP("Test environment not available");
    X11Connection conn;
    REQUIRE(conn.ok());
    auto config = [](uint32_t border)
    {
        return "[appearance]\nborder_width = " + std::to_string(border) + "\n[[rules]]\napply.floating = true\n";
    };
    LwmProcess wm(server.display(), config(0));
    REQUIRE(wait_for_wm_ready(conn, std::chrono::seconds(2)));
    auto window = create_window(conn, 10, 20, 3, 3);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, std::chrono::seconds(2)));
    auto initial = require_window_geometry(conn, window);
    REQUIRE(initial.width == 3);
    REQUIRE(initial.height == 3);
    REQUIRE(wm.write_config(config(65535)));
    ipc_ok("reload-config");
    REQUIRE(wait_for_window_geometry(conn, window, initial.x, initial.y, 1, 1));
    CHECK(get_window_border_width(conn, window) == 1);
    uint32_t x = initial.x + 1;
    xcb_configure_window(conn.get(), window, XCB_CONFIG_WINDOW_X, &x);
    observe_title_after_events(conn, window);
    CHECK(require_window_geometry(conn, window) == WindowGeometry{ static_cast<int16_t>(x), initial.y, 1, 1 });
    CHECK(get_window_border_width(conn, window) == 1);
    REQUIRE(wm.write_config(config(2)));
    ipc_ok("reload-config");
    uint32_t width = 9;
    xcb_configure_window(conn.get(), window, XCB_CONFIG_WINDOW_WIDTH, &width);
    observe_title_after_events(conn, window);
    CHECK(require_window_geometry(conn, window) == WindowGeometry{ static_cast<int16_t>(x), initial.y, 9, 1 });
    CHECK(get_window_border_width(conn, window) == 2);
    destroy_window(conn, window);
}

TEST_CASE("Integration: cold client-list publication interleaves every managed role", "[integration][placement][registration]")
{
    auto& server = X11TestEnvironment::instance();
    if (!server.available()) SKIP("Test environment not available");
    X11Connection conn;
    REQUIRE(conn.ok());
    std::vector<xcb_window_t> windows;
    for (auto type : { "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_WINDOW_TYPE_DOCK",
                       "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_NORMAL" })
    {
        auto window = create_window(conn, 10, 10, 100, 100);
        REQUIRE(set_window_type(conn, window, intern_atom(conn.get(), type)));
        map_window(conn, window);
        windows.push_back(window);
    }
    REQUIRE(get_window_geometry(conn, windows.back()));
    LwmProcess wm(server.display());
    REQUIRE(wait_for_wm_ready(conn, std::chrono::seconds(2)));
    auto list = intern_atom(conn.get(), "_NET_CLIENT_LIST");
    CHECK(get_window_property_windows(conn.get(), conn.root(), list) == windows);
    for (auto window : windows) destroy_window(conn, window);
}
