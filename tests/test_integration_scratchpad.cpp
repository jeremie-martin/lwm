#include "restart_handoff.hpp"
#include <X11/Xlib.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <sys/socket.h>
#include <sys/un.h>
#include <xcb/xtest.h>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

std::string scratchpad_match_config()
{
    return R"(
[commands]
terminal = { argv = ["/bin/true"] }

[workspaces]
count = 1
names = ["1"]

[[scratchpads]]
name = "terminal"
spawn = { ref = "terminal" }
match = { class = "ScratchpadClass", instance = "scratchpad-instance" }
size = { width = 0.8, height = 0.6 }
)";
}

std::string title_scratchpad_match_config()
{
    return R"(
[commands]
terminal = { argv = ["/bin/true"] }

[workspaces]
count = 1
names = ["1"]

[[scratchpads]]
name = "terminal"
spawn = { ref = "terminal" }
match = { class = "ScratchpadClass", title = "dropdown" }
size = { width = 0.8, height = 0.6 }
)";
}

} // namespace

TEST_CASE(
    "Integration: named scratchpads match WM_CLASS class and instance in the documented order",
    "[integration][scratchpad]"
)
{
    auto test_env = TestEnvironment::create(scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t window = create_window(conn, 10, 10, 240, 160);
    set_window_wm_class(conn, window, "scratchpad-instance", "ScratchpadClass");
    map_window(conn, window);

    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, window); }, kTimeout));

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: named scratchpads can finish a pending launch after a late title update",
    "[integration][scratchpad]"
)
{
    auto test_env = TestEnvironment::create(title_scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto toggle_result = send_ipc_command("scratchpad toggle terminal");
    REQUIRE(toggle_result.has_value());
    REQUIRE(*toggle_result == "ok");

    xcb_window_t window = create_window(conn, 10, 10, 240, 160);
    set_window_wm_class(conn, window, "scratchpad-instance", "ScratchpadClass");
    map_window(conn, window);

    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    set_window_title(conn, window, "dropdown");
    bool shown = wait_for_window_geometry(conn, window, 128, 144, 1020, 428);
    INFO(test_env->wm.diagnostics());
    INFO(send_ipc_command("window list").value_or("no window reply"));
    INFO(send_ipc_command("scratchpad list").value_or("no scratchpad reply"));
    REQUIRE(shown);

    destroy_window(conn, window);
}

TEST_CASE(
    "Integration: scratchpad regains focus on pointer enter after keyboard focus change",
    "[integration][scratchpad]"
)
{
    auto test_env = TestEnvironment::create(scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    // Create a tiled window as background
    xcb_window_t tiled = create_window(conn, 10, 10, 400, 300);
    set_window_wm_class(conn, tiled, "tiled-inst", "TiledClass");
    map_window(conn, tiled);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    // Trigger scratchpad toggle → pending launch
    auto toggle_result = send_ipc_command("scratchpad toggle terminal");
    REQUIRE(toggle_result.has_value());
    REQUIRE(*toggle_result == "ok");

    // Create scratchpad window that selects ButtonPress (like Iced/winit apps).
    // ButtonPress is exclusive in X11 — if the WM also selects it, the WM's
    // ChangeWindowAttributes fails atomically with BadAccess, dropping all
    // event selections including ENTER_WINDOW and breaking focus-follows-mouse.
    xcb_window_t sp = create_window(conn, 10, 10, 240, 160);
    set_window_wm_class(conn, sp, "scratchpad-instance", "ScratchpadClass");
    uint32_t app_mask = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn.get(), sp, XCB_CW_EVENT_MASK, &app_mask);
    map_window(conn, sp);
    REQUIRE(wait_for_active_window(conn, sp, kTimeout));
    auto sp_geom = get_window_geometry(conn, sp);
    REQUIRE(sp_geom.has_value());
    REQUIRE(sp_geom->x >= 0);

    // Use _NET_ACTIVE_WINDOW to focus the tiled window (simulating keyboard focus change)
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    REQUIRE(net_active_window != XCB_NONE);
    send_client_message(conn, tiled, net_active_window, 2, XCB_CURRENT_TIME, 0);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    // Scratchpad should still be on-screen (not hidden)
    {
        auto geom = get_window_geometry(conn, sp);
        REQUIRE(geom.has_value());
        REQUIRE(geom->x >= 0);
    }

    // Warp to root (away), then to scratchpad center — should trigger EnterNotify and refocus
    xcb_warp_pointer(conn.get(), XCB_NONE, conn.root(), 0, 0, 0, 0, 0, 0);
    observe_title_after_events(conn, tiled);

    int16_t sp_cx = static_cast<int16_t>(sp_geom->x + sp_geom->width / 2);
    int16_t sp_cy = static_cast<int16_t>(sp_geom->y + sp_geom->height / 2);
    xcb_warp_pointer(conn.get(), XCB_NONE, conn.root(), 0, 0, 0, 0, sp_cx, sp_cy);
    xcb_flush(conn.get());
    REQUIRE(wait_for_active_window(conn, sp, kTimeout));

    destroy_window(conn, sp);
    destroy_window(conn, tiled);
}

TEST_CASE("Integration: floating scratchpad preserves kind and geometry across restart", "[integration][scratchpad][restart]")
{
    auto test_env = TestEnvironment::create(scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    // Create a tiled window so the scratchpad has something to be compared against
    xcb_window_t tiled = create_window(conn, 10, 10, 400, 300);
    set_window_wm_class(conn, tiled, "tiled-inst", "TiledClass");
    map_window(conn, tiled);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    // Launch scratchpad
    auto toggle_result = send_ipc_command("scratchpad toggle terminal");
    REQUIRE(toggle_result.has_value());
    REQUIRE(*toggle_result == "ok");

    // Create and map scratchpad window
    xcb_window_t sp = create_window(conn, 10, 10, 240, 160);
    set_window_wm_class(conn, sp, "scratchpad-instance", "ScratchpadClass");
    map_window(conn, sp);
    REQUIRE(wait_for_active_window(conn, sp, kTimeout));

    // Record the geometry the WM assigned (centered floating)
    auto geom_before = get_window_geometry(conn, sp);
    REQUIRE(geom_before.has_value());
    REQUIRE(geom_before->x >= 0);

    auto previous = wm_instance(conn);
    REQUIRE(previous);
    REQUIRE(send_ipc_command("restart") == "ok");
    REQUIRE(wait_for_wm_restart(conn, std::chrono::seconds(5), *previous));
    REQUIRE(wait_for_active_window(conn, sp, kTimeout));
    REQUIRE(get_window_geometry(conn, sp) == geom_before);
    auto kind = intern_atom(conn.get(), "_LWM_WINDOW_CLASS");
    REQUIRE(get_window_property_string(conn.get(), sp, kind) == "floating");

    destroy_window(conn, sp);
    destroy_window(conn, tiled);
}

TEST_CASE("Integration: scratchpad cycle keeps pooled windows in rotation", "[integration][scratchpad]")
{
    auto test_env = TestEnvironment::create(scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t first = create_window(conn, 10, 10, 300, 220);
    set_window_wm_class(conn, first, "pool-first", "PoolWindow");
    map_window(conn, first);
    REQUIRE(wait_for_active_window(conn, first, kTimeout));

    xcb_window_t second = create_window(conn, 40, 40, 320, 240);
    set_window_wm_class(conn, second, "pool-second", "PoolWindow");
    map_window(conn, second);
    REQUIRE(wait_for_active_window(conn, second, kTimeout));

    auto stash_second = send_ipc_command("scratchpad stash");
    REQUIRE(stash_second.has_value());
    REQUIRE(*stash_second == "ok");
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, second); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, first, kTimeout));

    auto stash_first = send_ipc_command("scratchpad stash");
    REQUIRE(stash_first.has_value());
    REQUIRE(*stash_first == "ok");
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, first); }, kTimeout));

    auto first_cycle = send_ipc_command("scratchpad cycle");
    REQUIRE(first_cycle.has_value());
    REQUIRE(*first_cycle == "ok");
    REQUIRE(wait_for_active_window(conn, first, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, first); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, second); }, kTimeout));

    auto second_cycle = send_ipc_command("scratchpad cycle");
    REQUIRE(second_cycle.has_value());
    REQUIRE(*second_cycle == "ok");
    REQUIRE(wait_for_active_window(conn, second, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, first); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, second); }, kTimeout));

    auto third_cycle = send_ipc_command("scratchpad cycle");
    REQUIRE(third_cycle.has_value());
    REQUIRE(*third_cycle == "ok");
    REQUIRE(wait_for_active_window(conn, first, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, first); }, kTimeout));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, second); }, kTimeout));

    destroy_window(conn, second);
    destroy_window(conn, first);
}

TEST_CASE("Integration: tiled scratchpad pool preserves prior floating geometry", "[integration][scratchpad][floating]")
{
    auto test_env = TestEnvironment::create(scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    if (!extension_available(conn, &xcb_test_id))
        SKIP("XTEST extension not available");


    xcb_atom_t net_moveresize_window = intern_atom(conn.get(), "_NET_MOVERESIZE_WINDOW");
    REQUIRE(net_moveresize_window != XCB_NONE);

    xcb_window_t window = create_window(conn, 10, 10, 300, 220);
    set_window_wm_class(conn, window, "pool-prior", "PoolWindow");
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    auto toggle_float_at_window_center = [&]()
    {
        auto geometry = get_window_geometry(conn, window);
        REQUIRE(geometry.has_value());

        int16_t center_x = static_cast<int16_t>(geometry->x + geometry->width / 2);
        int16_t center_y = static_cast<int16_t>(geometry->y + geometry->height / 2);
        return send_mouse_chord(conn, XStringToKeysym("Super_L"), XCB_BUTTON_INDEX_2, center_x, center_y);
    };

    REQUIRE(toggle_float_at_window_center());

    WindowGeometry saved_float{ 123, 87, 345, 234 };
    constexpr uint32_t move_resize_flags = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11);
    send_client_message(
        conn,
        window,
        net_moveresize_window,
        move_resize_flags,
        static_cast<uint32_t>(saved_float.x),
        static_cast<uint32_t>(saved_float.y),
        saved_float.width,
        saved_float.height
    );
    REQUIRE(wait_for_window_geometry(conn, window, saved_float.x, saved_float.y, saved_float.width, saved_float.height)
    );

    REQUIRE(toggle_float_at_window_center());
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, window);
            return geometry.has_value() && *geometry != saved_float;
        },
        kTimeout
    ));

    auto stash = send_ipc_command("scratchpad stash");
    REQUIRE(stash.has_value());
    REQUIRE(*stash == "ok");
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, window); }, kTimeout));

    auto show = send_ipc_command("scratchpad cycle");
    REQUIRE(show.has_value());
    REQUIRE(*show == "ok");
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, window); }, kTimeout));

    REQUIRE(toggle_float_at_window_center());
    WindowGeometry final_geometry{};
    bool restored_saved_geometry = wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, window);
            if (!geometry)
                return false;
            final_geometry = *geometry;
            return *geometry == saved_float;
        },
        kTimeout
    );
    INFO(
        "restored floating geometry was " << final_geometry.x << "," << final_geometry.y << " " << final_geometry.width
                                          << "x" << final_geometry.height
    );
    REQUIRE(restored_saved_geometry);

    destroy_window(conn, window);
}

TEST_CASE("Integration: visible scratchpad pool window keeps cycling after restart", "[integration][scratchpad][restart]")
{
    auto test_env = TestEnvironment::create(scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t pooled = create_window(conn, 10, 10, 300, 220);
    set_window_wm_class(conn, pooled, "pool-visible", "PoolWindow");
    map_window(conn, pooled);
    REQUIRE(wait_for_active_window(conn, pooled, kTimeout));

    auto stash = send_ipc_command("scratchpad stash");
    REQUIRE(stash.has_value());
    REQUIRE(*stash == "ok");
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, pooled); }, kTimeout));

    auto show = send_ipc_command("scratchpad cycle");
    REQUIRE(show.has_value());
    REQUIRE(*show == "ok");
    REQUIRE(wait_for_active_window(conn, pooled, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, pooled); }, kTimeout));

    auto previous_wm = wm_instance(conn);
    REQUIRE(previous_wm.has_value());
    auto restart_result = send_ipc_command("restart");
    (void)restart_result;
    REQUIRE(wait_for_wm_restart(conn, std::chrono::seconds(5), *previous_wm));
    REQUIRE(wait_for_active_window(conn, pooled, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, pooled); }, kTimeout));

    auto hide = send_ipc_command("scratchpad cycle");
    REQUIRE(hide.has_value());
    REQUIRE(*hide == "ok");
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, pooled); }, kTimeout));

    destroy_window(conn, pooled);
}

TEST_CASE("Integration: hidden scratchpad stays hidden across restart", "[integration][scratchpad][restart]")
{
    auto test_env = TestEnvironment::create(scratchpad_match_config());
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_window_t tiled = create_window(conn, 10, 10, 400, 300);
    set_window_wm_class(conn, tiled, "tiled-inst", "TiledClass");
    map_window(conn, tiled);
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    auto first_toggle = send_ipc_command("scratchpad toggle terminal");
    REQUIRE(first_toggle.has_value());
    REQUIRE(*first_toggle == "ok");

    xcb_window_t sp = create_window(conn, 10, 10, 240, 160);
    set_window_wm_class(conn, sp, "scratchpad-instance", "ScratchpadClass");
    map_window(conn, sp);
    REQUIRE(wait_for_active_window(conn, sp, kTimeout));
    REQUIRE(wait_for_condition([&]() { return !is_hidden_offscreen(conn, sp); }, kTimeout));

    auto second_toggle = send_ipc_command("scratchpad toggle terminal");
    REQUIRE(second_toggle.has_value());
    REQUIRE(*second_toggle == "ok");
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, sp); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    auto previous_wm = wm_instance(conn);
    REQUIRE(previous_wm.has_value());
    auto restart_result = send_ipc_command("restart");
    (void)restart_result;
    REQUIRE(wait_for_wm_restart(conn, std::chrono::seconds(5), *previous_wm));
    REQUIRE(wait_for_condition([&]() { return is_hidden_offscreen(conn, sp); }, kTimeout));
    REQUIRE(wait_for_active_window(conn, tiled, kTimeout));

    destroy_window(conn, sp);
    destroy_window(conn, tiled);
}

TEST_CASE("Integration: scratchpad launch failure remains retryable", "[integration][scratchpad][launch]")
{
    auto env = TestEnvironment::create(R"(
[[scratchpads]]
name = "broken"
spawn = { argv = ["/definitely/missing/lwm-test-program"] }
match = { class = "LaunchTest" }
)");
    if (!env)
        SKIP("X11 unavailable");
    for (int i = 0; i < 2; ++i)
    {
        REQUIRE(send_ipc_command("scratchpad toggle broken") == "ok");
        auto state = send_ipc_command("scratchpad list");
        REQUIRE(state);
        CHECK(state->find("\"pending\":false") != std::string::npos);
    }
}

TEST_CASE(
    "Integration: pending launches require explicit cancellation and late windows remain claimable",
    "[integration][scratchpad][launch][sequence]"
)
{
    auto env = TestEnvironment::create(R"(
[[scratchpads]]
name = "late"
spawn = { shell = 'printf "launch\n" >> "$XDG_RUNTIME_DIR/launches"' }
match = { class = "LaunchTest" }
)");
    if (!env)
        SKIP("X11 unavailable");
    REQUIRE(send_ipc_command("scratchpad toggle late") == "ok");
    auto launches = std::filesystem::path(env->wm.runtime_dir()) / "launches";
    REQUIRE(wait_for_condition([&] { return read_text_file(launches) == "launch\n"; }, kTimeout));
    REQUIRE(send_ipc_command("scratchpad toggle late") == "ok");
    // The acknowledged toggle has returned from spawning. Wait for any child
    // to finish before counting invocations, so scheduling cannot hide a duplicate.
    auto children = std::filesystem::path("/proc") / std::to_string(env->wm.pid()) / "task"
        / std::to_string(env->wm.pid()) / "children";
    REQUIRE(std::filesystem::exists(children));
    REQUIRE(wait_for_condition([&] { return read_text_file(children).empty(); }, kTimeout));
    REQUIRE(read_text_file(launches) == "launch\n");
    auto state = send_ipc_command("scratchpad list");
    REQUIRE(state);
    CHECK(state->find("\"pending\":true") != std::string::npos);
    auto result = run_lwmctl(env->wm, { "scratchpad", "cancel-launch", "late" });
    REQUIRE(result);
    REQUIRE(result->exit_code == 0);
    state = send_ipc_command("scratchpad list");
    REQUIRE(state);
    CHECK(state->find("\"pending\":false") != std::string::npos);
    auto window = create_window(env->conn, 10, 10, 240, 160);
    set_window_wm_class(env->conn, window, "launch", "LaunchTest");
    map_window(env->conn, window);
    REQUIRE(wait_for_condition(
        [&]
        {
            auto response = send_ipc_command("scratchpad list");
            return response && response->find("\"window\":" + std::to_string(window)) != std::string::npos;
        },
        kTimeout
    ));
    REQUIRE(send_ipc_command("scratchpad toggle late") == "ok");
    REQUIRE(wait_for_condition([&] { return !is_hidden_offscreen(env->conn, window); }, kTimeout));
    destroy_window(env->conn, window);
}

TEST_CASE("Integration: spawned commands do not inherit WM descriptors", "[integration][launch]")
{
    auto env = TestEnvironment::create(R"(
[autostart]
commands = [{ shell = 'exec 3>"$XDG_RUNTIME_DIR/child-marker"; for fd in /proc/$$/fd/*; do case "${fd##*/}" in 0|1|2) continue;; esac; readlink "$fd" >> "$XDG_RUNTIME_DIR/child-fds"; done; printf done > "$XDG_RUNTIME_DIR/child-done"' }]
)");
    if (!env)
        SKIP("X11 unavailable");
    auto runtime = std::filesystem::path(env->wm.runtime_dir());
    REQUIRE(wait_for_condition([&] { return std::filesystem::exists(runtime / "child-done"); }, kTimeout));
    auto descriptors = read_text_file(runtime / "child-fds");
    REQUIRE(descriptors.find("child-marker") != std::string::npos);
    INFO(descriptors);
    CHECK(descriptors.find("socket:") == std::string::npos);
    CHECK(descriptors.find(".log") == std::string::npos);
}

TEST_CASE("Integration: pending scratchpad launches continue through restart", "[integration][scratchpad][restart]")
{
    bool failed_exec = GENERATE(false, true);
    auto env = TestEnvironment::create(R"(
[[scratchpads]]
name = "late"
spawn = { shell = 'printf "launch\n" >> "$XDG_RUNTIME_DIR/launches"' }
match = { class = "LaunchTest", title = "ready" }
)");
    REQUIRE(env);
    auto& conn = env->conn;
    ipc_ok("scratchpad toggle late");
    auto launches = std::filesystem::path(env->wm.runtime_dir()) / "launches";
    REQUIRE(wait_for_condition([&] { return read_text_file(launches) == "launch\n"; }, kTimeout));
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    ipc_ok(failed_exec ? "exec /definitely/missing/lwm" : "restart");
    REQUIRE(wait_for_wm_restart(conn, std::chrono::seconds(5), *previous));
    auto scratchpads = [&] { return ipc_json("scratchpad list"); };
    REQUIRE(scratchpads()["named"][0]["pending"] == true);
    ipc_ok("scratchpad toggle late");
    auto children = std::filesystem::path("/proc") / std::to_string(env->wm.pid()) / "task"
        / std::to_string(env->wm.pid()) / "children";
    REQUIRE(wait_for_condition([&] { return read_text_file(children).empty(); }, kTimeout));
    CHECK(read_text_file(launches) == "launch\n");
    auto arrival = GENERATE("map", "metadata", "cancelled");
    CAPTURE(failed_exec, arrival);
    bool cancelled = std::string_view(arrival) == "cancelled";
    bool late_metadata = std::string_view(arrival) == "metadata";
    if (cancelled)
    {
        ipc_ok("scratchpad cancel-launch late");
        REQUIRE(scratchpads()["named"][0]["pending"] == false);
    }
    auto window = create_window(conn, 10, 10, 240, 160);
    set_window_wm_class(conn, window, "launch", "LaunchTest");
    // A matching title arrives after registration; this must fulfill the same
    // pending request retained through exec, rather than launch another process.
    if (!late_metadata)
        set_window_title(conn, window, "ready");
    map_window(conn, window);
    if (late_metadata)
    {
        REQUIRE(wait_for_condition(
            [&]
            {
                return get_window_property_string(conn.get(), window, intern_atom(conn.get(), "_LWM_WINDOW_CLASS"))
                    == "tiled";
            },
            kTimeout
        ));
        set_window_title(conn, window, "ready");
    }
    REQUIRE(wait_for_condition([&] { return scratchpads()["named"][0]["window"] == window; }, kTimeout));
    CHECK(scratchpads()["named"][0]["pending"] == false);
    if (cancelled)
        REQUIRE(wait_for_condition([&] { return is_hidden_offscreen(conn, window); }, kTimeout));
    else
    {
        REQUIRE(wait_for_active_window(conn, window, kTimeout));
        CHECK_FALSE(is_hidden_offscreen(conn, window));
    }
    destroy_window(conn, window);
}

TEST_CASE("Integration: a pending scratchpad can arrive during exec handoff", "[integration][scratchpad][restart]")
{
    auto config = title_scratchpad_match_config();
    config += R"(
[[scratchpads]]
name = "second"
spawn = { ref = "terminal" }
match = { class = "ScratchpadClass", title = "dropdown" }
size = { width = 0.8, height = 0.6 }
)";
    auto env = TestEnvironment::create(config);
    REQUIRE(env);
    auto& conn = env->conn;
    auto claimed = create_window(conn, 20, 20, 240, 160);
    set_window_wm_class(conn, claimed, "scratchpad-instance", "ScratchpadClass");
    set_window_title(conn, claimed, "dropdown");
    map_window(conn, claimed);
    REQUIRE(wait_for_condition([&] { return is_hidden_offscreen(conn, claimed); }, kTimeout));
    ipc_ok("scratchpad toggle second");
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    PausedRestart restart(env->wm);
    auto window = create_window(conn, 10, 10, 240, 160);
    set_window_wm_class(conn, window, "scratchpad-instance", "ScratchpadClass");
    set_window_title(conn, window, "dropdown");
    map_window(conn, window);
    REQUIRE(get_window_geometry(conn, window)); // Server processed the map before adoption.
    restart.resume();
    REQUIRE(wait_for_wm_restart(conn, std::chrono::seconds(5), *previous));
    auto slots = ipc_json("scratchpad list")["named"];
    REQUIRE(slots.size() == 2);
    CHECK(slots[0]["window"] == claimed);
    CHECK(is_hidden_offscreen(conn, claimed));
    auto slot = slots[1];
    CHECK(slot["window"] == window);
    CHECK(slot["pending"] == false);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(wait_for_window_geometry(conn, window, 128, 144, 1020, 428));
    ipc_ok("scratchpad toggle second");
    REQUIRE(wait_for_condition([&] { return is_hidden_offscreen(conn, window); }, kTimeout));
    destroy_window(conn, window);
    destroy_window(conn, claimed);
}

TEST_CASE(
    "Integration: pool rotation reaches every member and survives restart and removal",
    "[integration][scratchpad][restart][pool-rotation]"
)
{
    auto env = TestEnvironment::create(scratchpad_match_config());
    REQUIRE(env);
    auto& conn = env->conn;
    std::vector<xcb_window_t> windows;
    for (int i = 0; i < 3; ++i)
    {
        auto window = create_window(conn, 10, 10, 240, 160);
        windows.push_back(window);
        map_window(conn, window);
        REQUIRE(wait_for_active_window(conn, window, kTimeout));
        if (i == 1)
            ipc_ok("window float");
        ipc_ok("scratchpad stash");
    }
    auto cycle_to = [&](size_t index)
    {
        ipc_ok("scratchpad cycle");
        REQUIRE(wait_for_active_window(conn, windows[index], kTimeout));
        for (size_t i = 0; i < windows.size(); ++i) REQUIRE(is_hidden_offscreen(conn, windows[i]) == (i != index));
    };
    cycle_to(2);
    cycle_to(1);

    auto instance = wm_instance(conn);
    REQUIRE(instance);
    ipc_ok("restart");
    REQUIRE(wait_for_wm_restart(conn, kTimeout, *instance));
    for (auto index : { 0, 2, 1, 0, 2 }) cycle_to(index);

    // Removing the selected entry leaves the next surviving member recallable.
    destroy_window(conn, windows[2]);
    REQUIRE(wait_for_condition([&] { return ipc_json("scratchpad list").at("pool").size() == 2; }, kTimeout));
    windows.pop_back();
    for (auto index : { 1, 0, 1 }) cycle_to(index);
    for (auto window : windows) destroy_window(conn, window);
}

TEST_CASE(
    "Integration: pool selection survives workspace hiding minimization and focus changes",
    "[integration][scratchpad][pool-rotation]"
)
{
    bool floating = GENERATE(false, true);
    auto hidden_by = GENERATE("workspace", "minimize");
    CAPTURE(floating, hidden_by);
    auto env = TestEnvironment::create("[workspaces]\ncount = 2\nnames = [\"1\", \"2\"]\n");
    REQUIRE(env);
    auto& conn = env->conn;
    ipc_ok("scratchpad cycle"); // Empty pool is a no-op.
    auto window = create_window(conn, 10, 10, 240, 160);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    if (floating)
        ipc_ok("window float");
    ipc_ok("scratchpad stash");
    ipc_ok("scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    if (std::string_view(hidden_by) == "workspace")
        ipc_ok("workspace switch 1");
    else
        send_client_message(conn, window, intern_atom(conn.get(), "WM_CHANGE_STATE"), 3);
    REQUIRE(wait_for_condition([&] { return is_hidden_offscreen(conn, window); }, kTimeout));
    ipc_ok("scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE_FALSE(is_hidden_offscreen(conn, window));
    auto desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    REQUIRE(wait_for_property_cardinal(
        conn.get(),
        window,
        desktop,
        std::string_view(hidden_by) == "workspace" ? 1 : 0,
        kTimeout
    ));

    // Losing focus does not advance or hide the current selection.
    auto other = create_window(conn, 10, 10, 200, 100);
    map_window(conn, other);
    REQUIRE(wait_for_active_window(conn, other, kTimeout));
    ipc_ok("scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    ipc_ok("scratchpad cycle");
    REQUIRE(wait_for_condition([&] { return is_hidden_offscreen(conn, window); }, kTimeout));
    ipc_ok("scratchpad cycle");
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    destroy_window(conn, window);
    destroy_window(conn, other);
}
