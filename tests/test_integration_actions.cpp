#include "ipc_subscription.hpp"
#include "wm_observations.hpp"
#include <catch2/generators/catch_generators.hpp>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

nlohmann::json window_entry(std::string const& path, xcb_window_t window)
{
    auto snapshot = ipc_json(path, "window list");
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
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    ipc_ok(*path, std::string("layout set ") + layout);

    auto first = create_window(conn, 10, 10, 200, 150);
    auto hidden = create_window(conn, 10, 10, 200, 150);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, kTimeout));
    map_window(conn, hidden);
    REQUIRE(wait_for_active_window(conn, hidden, kTimeout));
    ipc_ok(*path, "scratchpad stash");
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
        ipc_ok(*path, std::string("window swap ") + direction);
        CHECK(window_entry(*path, hidden).at("iconic") == true);
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
    auto env = TestEnvironment::create("[workspaces]\ncount = 3\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    auto a = create_window(conn, 10, 10, 200, 150);
    auto b = create_window(conn, 10, 10, 200, 150);
    map_window(conn, a);
    REQUIRE(wait_for_active_window(conn, a, kTimeout));
    map_window(conn, b);
    REQUIRE(wait_for_active_window(conn, b, kTimeout));
    auto kind = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");

    ipc_ok(*path, "window float");
    CHECK(get_window_property_string(conn.get(), b, kind) == "floating");
    ipc_ok(*path, "window float");
    CHECK(get_window_property_string(conn.get(), b, kind) == "tiled");

    ipc_ok(*path, "window fullscreen");
    CHECK(has_state(conn, b, intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN")));
    ipc_ok(*path, "window fullscreen");

    auto before = window_entry(*path, a);
    ipc_ok(*path, "window swap next");
    auto workspaces = ipc_json(*path, "workspace list");
    CHECK(workspaces.at("monitors")[0].at("workspaces")[0].at("window_count") == 2);

    ipc_ok(*path, "window to-workspace 2");
    CHECK(window_entry(*path, b).at("workspace") == 2);
    REQUIRE(wait_for_active_window(conn, a, kTimeout));

    // Toggling returns to the previous workspace, where the moved window is focused.
    ipc_ok(*path, "workspace switch 2");
    REQUIRE(wait_for_active_window(conn, b, kTimeout));
    CHECK(ipc_ok(*path, "workspace toggle") == "ok 0");
    REQUIRE(wait_for_active_window(conn, a, kTimeout));

    // A single monitor has no neighbor; these are successful no-ops.
    ipc_ok(*path, "monitor focus right");
    ipc_ok(*path, "window to-monitor left");
    CHECK(window_entry(*path, a).at("monitor") == 0);
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
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    // The victim lives on its own connection: without WM_DELETE_WINDOW, close kills its client.
    X11Connection victim_connection;
    REQUIRE(victim_connection.ok());
    auto victim = create_window(victim_connection, 10, 10, 200, 150);
    map_window(victim_connection, victim);
    REQUIRE(wait_for_active_window(conn, victim, kTimeout));
    ipc_ok(*path, "window close");
    REQUIRE(wait_for_condition([&] { return window_entry(*path, victim).is_null(); }, kTimeout));
    auto reply = send_ipc_command(*path, "window close");
    REQUIRE(reply);
    CHECK(*reply == "error no active window");
}

TEST_CASE("Integration: layout changes report the action that caused them", "[integration][ipc][subscribe]")
{
    auto env = TestEnvironment::create("[layout]\nmin_ratio = 0.1\n[appearance]\npadding = 10\nborder_width = 1\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    auto first = create_window(conn, 10, 10, 200, 200);
    auto second = create_window(conn, 10, 10, 200, 200);
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, kTimeout));
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, kTimeout));
    Subscriber subscriber(*path, "layout_change");

    ipc_ok(*path, "ratio adjust 0.05");
    auto adjusted = subscriber.event();
    CHECK(adjusted.at("action") == "adjust_ratio");
    CHECK(adjusted.at("delta") == 0.05);
    ipc_ok(*path, "layout set monocle");
    auto layout = subscriber.event();
    CHECK(layout.at("action") == "set_layout");
    CHECK(layout.at("value") == "monocle");
    ipc_ok(*path, "layout set master-stack");
    subscriber.event();

    // A pointer drag of the split reports its final ratio when released.
    auto left = get_window_geometry(conn, first);
    auto right = get_window_geometry(conn, second);
    REQUIRE(left);
    REQUIRE(right);
    if (left->x > right->x)
        std::swap(left, right);
    int16_t x = static_cast<int16_t>((left->x + left->width + right->x) / 2);
    int16_t y = static_cast<int16_t>(left->y + left->height / 2);
    send_pointer_event(conn, XCB_BUTTON_PRESS, x, y);
    send_pointer_event(conn, XCB_MOTION_NOTIFY, static_cast<int16_t>(x + 60), y);
    send_pointer_event(conn, XCB_BUTTON_RELEASE, static_cast<int16_t>(x + 60), y);
    auto resized = subscriber.event();
    CHECK(resized.at("action") == "resize_split");
    CHECK(resized.at("value").get<double>() > 0.55);
    destroy_window(conn, second);
    destroy_window(conn, first);
}

TEST_CASE("Integration: state_change fires only when the exposed state changes", "[integration][ipc][subscribe]")
{
    auto env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    auto window = create_window(conn, 10, 10, 200, 150);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    Subscriber subscriber(*path, "state_change");
    // Queries and no-op actions leave the exposed state unchanged.
    ipc_ok(*path, "window list");
    ipc_ok(*path, "workspace switch 0");
    ipc_ok(*path, "ratio reset");
    CHECK_FALSE(subscriber.reader.read(subscriber.fd, std::chrono::milliseconds(50)));
    ipc_ok(*path, "workspace switch 1");
    CHECK(subscriber.event().at("event") == "state_change");
    ipc_ok(*path, "workspace switch 1");
    CHECK_FALSE(subscriber.reader.read(subscriber.fd, std::chrono::milliseconds(50)));
    ipc_ok(*path, "workspace switch 0");
    CHECK(subscriber.event().at("event") == "state_change");
    CHECK_FALSE(subscriber.reader.read(subscriber.fd, std::chrono::milliseconds(50)));
    destroy_window(conn, window);
}

TEST_CASE("Integration: pooled tiles stay tiled and consistent while hidden", "[integration][scratchpad]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    auto keep = create_window(conn, 10, 10, 200, 150);
    auto pooled = create_window(conn, 10, 10, 200, 150);
    map_window(conn, keep);
    REQUIRE(wait_for_active_window(conn, keep, kTimeout));
    map_window(conn, pooled);
    REQUIRE(wait_for_active_window(conn, pooled, kTimeout));

    ipc_ok(*path, "scratchpad stash");
    REQUIRE(wait_for_active_window(conn, keep, kTimeout));
    auto hidden = window_entry(*path, pooled);
    CHECK(hidden.at("kind") == "tiled");
    CHECK(hidden.at("iconic") == true);
    CHECK(is_hidden_offscreen(conn, pooled));

    // Activation by a pager shows the tile in place; it stays in the pool.
    auto active = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    send_client_message(conn, pooled, active, 2, XCB_CURRENT_TIME);
    REQUIRE(wait_for_active_window(conn, pooled, kTimeout));
    CHECK(window_entry(*path, pooled).at("iconic") == false);
    auto pool = ipc_json(*path, "scratchpad list").at("pool");
    CHECK(pool == nlohmann::json::array({ pooled }));

    // Cycling hides the visible pooled window again, then recalls it.
    ipc_ok(*path, "scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, keep, kTimeout));
    CHECK(window_entry(*path, pooled).at("iconic") == true);
    ipc_ok(*path, "scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, pooled, kTimeout));
    CHECK(window_entry(*path, pooled).at("kind") == "tiled");
    destroy_window(conn, pooled);
    destroy_window(conn, keep);
}

TEST_CASE("Integration: dock and desktop events carry no workspace", "[integration][subscribe]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    Subscriber subscriber(*path, "window_map,window_unmap");
    auto dock = create_window(conn, 0, 0, 800, 20);
    REQUIRE(set_window_type(conn, dock, intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DOCK")));
    map_window(conn, dock);
    auto mapped = subscriber.event();
    CHECK(mapped.at("event") == "window_map");
    CHECK(mapped.at("kind") == "dock");
    CHECK_FALSE(mapped.contains("workspace"));
    destroy_window(conn, dock);
    auto unmapped = subscriber.event();
    CHECK(unmapped.at("event") == "window_unmap");
    CHECK(unmapped.at("kind") == "dock");
    CHECK_FALSE(unmapped.contains("monitor"));
}
