#include "x11_test_harness.hpp"
#include "wm_observations.hpp"
#include <catch2/generators/catch_generators.hpp>
#include <thread>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

nlohmann::json window_entry(xcb_window_t window)
{
    auto snapshot = ipc_json("window list");
    for (auto const& entry : snapshot.at("windows"))
        if (entry.at("id") == window)
            return entry;
    return nullptr;
}

} // namespace

TEST_CASE("Integration: tile swaps skip stashed windows", "[integration][ipc][actions][scratchpad]")
{
    auto layout = GENERATE("monocle", "master-stack");
    auto direction = GENERATE("next", "prev");
    auto extra_tile = GENERATE(false, true);
    CAPTURE(layout, direction, extra_tile);
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    ipc_ok(std::string("layout set ") + layout);

    auto first = create_window(conn, 10, 10, 200, 150);
    auto hidden = create_window(conn, 10, 10, 200, 150);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, kTimeout));
    map_window(conn, hidden);
    REQUIRE(wait_for_active_window(conn, hidden, kTimeout));
    ipc_ok("scratchpad stash");
    REQUIRE(wait_for_active_window(conn, first, kTimeout));
    REQUIRE(is_hidden_offscreen(conn, hidden));

    xcb_window_t active = first;
    if (extra_tile)
    {
        active = create_window(conn, 10, 10, 200, 150);
        map_window(conn, active);
        REQUIRE(wait_for_active_window(conn, active, kTimeout));
    }
    auto before = get_window_geometry(conn, first);
    REQUIRE(before);
    // Repeated swaps exercise both adjacency and wraparound past the hidden slot.
    for (int i = 0; i < 2; ++i)
    {
        ipc_ok(std::string("window swap ") + direction);
        CHECK(window_entry(hidden).at("iconic") == true);
        CHECK(is_hidden_offscreen(conn, hidden));
        auto expected = extra_tile && std::string_view(layout) == "monocle" && i == 0 ? first : active;
        REQUIRE(wait_for_active_window(conn, expected, kTimeout));
        if (extra_tile && std::string_view(layout) == "master-stack")
        {
            REQUIRE(wait_for_condition(
                [&]
                {
                    auto after = get_window_geometry(conn, first);
                    return after && ((after->x != before->x) == (i == 0));
                },
                kTimeout
            ));
        }
    }

    if (extra_tile)
        destroy_window(conn, active);
    destroy_window(conn, hidden);
    destroy_window(conn, first);
}

TEST_CASE("Integration: IPC window actions execute the key-binding operations", "[integration][ipc][actions]")
{
    auto env = TestEnvironment::create("[workspaces]\nnames = [\"1\", \"2\", \"3\"]\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto a = create_window(conn, 10, 10, 200, 150);
    auto b = create_window(conn, 10, 10, 200, 150);
    map_window(conn, a);
    REQUIRE(wait_for_active_window(conn, a, kTimeout));
    map_window(conn, b);
    REQUIRE(wait_for_active_window(conn, b, kTimeout));
    auto kind = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");

    ipc_ok("window float");
    CHECK(get_window_property_string(conn.get(), b, kind) == "floating");
    ipc_ok("window float");
    CHECK(get_window_property_string(conn.get(), b, kind) == "tiled");

    ipc_ok("window fullscreen");
    CHECK(has_state(conn, b, intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN")));
    ipc_ok("window fullscreen");

    auto before = window_entry(a);
    ipc_ok("window swap next");
    auto workspaces = ipc_json("workspace list");
    CHECK(workspaces.at("monitors")[0].at("workspaces")[0].at("window_count") == 2);

    ipc_ok("window to-workspace 2");
    CHECK(window_entry(b).at("workspace") == 2);
    REQUIRE(wait_for_active_window(conn, a, kTimeout));

    // Toggling returns to the previous workspace, where the moved window is focused.
    ipc_ok("workspace switch 2");
    REQUIRE(wait_for_active_window(conn, b, kTimeout));
    CHECK(ipc_ok("workspace toggle") == "ok");
    REQUIRE(wait_for_active_window(conn, a, kTimeout));

    // A single monitor has no neighbor; these are successful no-ops.
    ipc_ok("monitor focus right");
    ipc_ok("window to-monitor left");
    CHECK(window_entry(a).at("monitor") == 0);
    CHECK(before.at("id") == a);

    destroy_window(conn, b);
    destroy_window(conn, a);
}

TEST_CASE("Integration: window close reaches clients without the delete protocol", "[integration][ipc][actions]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    // The victim lives on its own connection: without WM_DELETE_WINDOW, close kills its client.
    X11Connection victim_connection;
    REQUIRE(victim_connection.ok());
    auto victim = create_window(victim_connection, 10, 10, 200, 150);
    map_window(victim_connection, victim);
    REQUIRE(wait_for_active_window(conn, victim, kTimeout));
    ipc_ok("window close");
    REQUIRE(wait_for_condition([&] { return window_entry(victim).is_null(); }, kTimeout));
    auto reply = send_ipc_command("window close");
    REQUIRE(reply);
    CHECK(*reply == "error no active window");
}

TEST_CASE("Integration: closing asks first and closing again kills", "[integration][ipc][actions][close]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    bool ping = GENERATE(false, true);
    CAPTURE(ping);
    X11Connection victim_connection;
    REQUIRE(victim_connection.ok());
    auto* c = victim_connection.get();
    auto protocols = intern_atom(c, "WM_PROTOCOLS");
    auto delete_window = intern_atom(c, "WM_DELETE_WINDOW");
    auto ping_atom = intern_atom(c, "_NET_WM_PING");
    auto victim = create_window(victim_connection, 10, 10, 200, 150);
    std::vector<xcb_atom_t> supported{ delete_window };
    if (ping)
        supported.push_back(ping_atom);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, victim, protocols, XCB_ATOM_ATOM, 32, supported.size(), supported.data());
    map_window(victim_connection, victim);
    REQUIRE(wait_for_active_window(conn, victim, kTimeout));
    std::vector<xcb_client_message_event_t> messages;
    auto received = [&](xcb_atom_t protocol)
    {
        auto find = [&] { return std::ranges::find_if(messages, [&](auto const& m) { return m.data.data32[0] == protocol; }); };
        wait_for_condition(
            [&]
            {
                while (auto* event = xcb_poll_for_event(c))
                {
                    auto const& m = reinterpret_cast<xcb_client_message_event_t const&>(*event);
                    if ((event->response_type & ~0x80) == XCB_CLIENT_MESSAGE && m.type == protocols)
                        messages.push_back(m);
                    free(event);
                }
                return find() != messages.end();
            },
            kTimeout
        );
        return find() == messages.end() ? std::nullopt : std::optional{ *find() };
    };
    ipc_ok("window close");
    REQUIRE(received(delete_window));
    if (ping)
    {
        auto request = received(ping_atom);
        REQUIRE(request);
        request->window = victim_connection.root();
        xcb_send_event(c, 0, victim_connection.root(), XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, reinterpret_cast<char const*>(&*request));
        xcb_flush(c);
    }
    // Past the deadline: a client without ping, or one that answered it, may be
    // showing a save dialog and is not killed automatically.
    std::this_thread::sleep_for(std::chrono::milliseconds(5500));
    CHECK_FALSE(window_entry(victim).is_null());
    ipc_ok("window close");
    REQUIRE(wait_for_condition([&] { return window_entry(victim).is_null(); }, kTimeout));
}

TEST_CASE("Integration: a client that ignores a close ping is killed at the deadline", "[integration][ipc][actions][close]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    X11Connection victim_connection;
    REQUIRE(victim_connection.ok());
    auto* c = victim_connection.get();
    auto victim = create_window(victim_connection, 10, 10, 200, 150);
    xcb_atom_t supported[] = { intern_atom(c, "WM_DELETE_WINDOW"), intern_atom(c, "_NET_WM_PING") };
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, victim, intern_atom(c, "WM_PROTOCOLS"), XCB_ATOM_ATOM, 32, 2, supported);
    map_window(victim_connection, victim);
    REQUIRE(wait_for_active_window(conn, victim, kTimeout));
    ipc_ok("window close");
    REQUIRE(wait_for_condition([&] { return window_entry(victim).is_null(); }, std::chrono::seconds(8)));
}

TEST_CASE("Integration: pooled tiles stay tiled and consistent while hidden", "[integration][scratchpad]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto keep = create_window(conn, 10, 10, 200, 150);
    auto pooled = create_window(conn, 10, 10, 200, 150);
    map_window(conn, keep);
    REQUIRE(wait_for_active_window(conn, keep, kTimeout));
    map_window(conn, pooled);
    REQUIRE(wait_for_active_window(conn, pooled, kTimeout));

    ipc_ok("scratchpad stash");
    REQUIRE(wait_for_active_window(conn, keep, kTimeout));
    auto hidden = window_entry(pooled);
    CHECK(hidden.at("kind") == "tiled");
    CHECK(hidden.at("iconic") == true);
    CHECK(is_hidden_offscreen(conn, pooled));

    // Activation by a pager shows the tile in place; it stays in the pool.
    auto active = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    send_client_message(conn, pooled, active, 2, XCB_CURRENT_TIME);
    REQUIRE(wait_for_active_window(conn, pooled, kTimeout));
    CHECK(window_entry(pooled).at("iconic") == false);
    auto pool = ipc_json("scratchpad list").at("pool");
    CHECK(pool == nlohmann::json::array({ pooled }));

    // Cycling hides the visible pooled window again, then recalls it.
    ipc_ok("scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, keep, kTimeout));
    CHECK(window_entry(pooled).at("iconic") == true);
    ipc_ok("scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, pooled, kTimeout));
    CHECK(window_entry(pooled).at("kind") == "tiled");
    destroy_window(conn, pooled);
    destroy_window(conn, keep);
}

