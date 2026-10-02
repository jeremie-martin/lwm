#include "restart_handoff.hpp"
#include <X11/Xlib.h>
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using namespace lwm::test;

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

using JsonValue = nlohmann::json;

std::optional<JsonValue> parse_ok_json(std::string const& reply)
{
    if (!reply.starts_with("ok ") || reply.size() < 5 || reply.back() != '\n')
        return std::nullopt;
    auto value = JsonValue::parse(std::string_view(reply).substr(3, reply.size() - 4), nullptr, false);
    return value.is_discarded() ? std::nullopt : std::optional{ std::move(value) };
}

JsonValue const* json_member(JsonValue const& value, std::string_view name)
{
    if (!value.is_object())
        return nullptr;
    auto it = value.find(std::string(name));
    return it == value.end() ? nullptr : &*it;
}

bool has_json_fields(
    JsonValue const& value,
    std::initializer_list<std::pair<std::string_view, JsonValue::value_t>> fields
)
{
    for (auto const& [name, type] : fields)
    {
        auto const* member = json_member(value, name);
        if (!member || member->type() != type)
            return false;
    }
    return true;
}

bool has_object_array_fields(
    JsonValue const& value,
    std::string_view array_name,
    std::initializer_list<std::pair<std::string_view, JsonValue::value_t>> fields
)
{
    auto const* array = json_member(value, array_name);
    if (!array || !array->is_array() || array->empty())
        return false;
    for (auto const& item : *array)
    {
        if (!has_json_fields(item, fields))
            return false;
    }
    return true;
}

bool has_typed_array(JsonValue const& value, std::string_view array_name, JsonValue::value_t item_type)
{
    auto const* array = json_member(value, array_name);
    if (!array || !array->is_array() || array->empty())
        return false;
    return std::all_of(
        array->begin(),
        array->end(),
        [item_type](JsonValue const& item) { return item.type() == item_type; }
    );
}

bool workspace_list_has_documented_shape(JsonValue const& value)
{
    if (!has_json_fields(
            value,
            {
                { "focused_monitor", JsonValue::value_t::number_unsigned },
                {        "monitors",           JsonValue::value_t::array }
    }
        ))
        return false;
    auto const* monitors = json_member(value, "monitors");
    if (!monitors || monitors->empty())
        return false;
    for (auto const& monitor : *monitors)
    {
        if (!has_json_fields(
                monitor,
                {
                    {             "index", JsonValue::value_t::number_unsigned },
                    {              "name",          JsonValue::value_t::string },
                    { "current_workspace", JsonValue::value_t::number_unsigned },
                    {        "workspaces",           JsonValue::value_t::array },
        }
            )
            || !has_object_array_fields(
                monitor,
                "workspaces",
                {
                    { "index", JsonValue::value_t::number_unsigned },
                    { "name", JsonValue::value_t::string },
                    { "current", JsonValue::value_t::boolean },
                    { "window_count", JsonValue::value_t::number_unsigned },
                    { "layout", JsonValue::value_t::string },
                }
            ))
        {
            return false;
        }
    }
    return true;
}

bool window_list_has_documented_shape(JsonValue const& value)
{
    return has_json_fields(
               value,
               {
                   { "focused", JsonValue::value_t::number_unsigned },
                   { "windows",           JsonValue::value_t::array }
    }
           )
        && has_object_array_fields(
               value,
               "windows",
               {
                   { "id", JsonValue::value_t::number_unsigned },
                   { "monitor", JsonValue::value_t::number_unsigned },
                   { "workspace", JsonValue::value_t::number_unsigned },
                   { "kind", JsonValue::value_t::string },
                   { "class", JsonValue::value_t::string },
                   { "instance", JsonValue::value_t::string },
                   { "title", JsonValue::value_t::string },
                   { "focused", JsonValue::value_t::boolean },
                   { "fullscreen", JsonValue::value_t::boolean },
                   { "urgent", JsonValue::value_t::boolean },
                   { "sticky", JsonValue::value_t::boolean },
                   { "iconic", JsonValue::value_t::boolean },
               }
        );
}

bool scratchpad_list_has_documented_shape(JsonValue const& value)
{
    return has_json_fields(
               value,
               {
                   { "named", JsonValue::value_t::array },
                   {  "pool", JsonValue::value_t::array }
    }
           )
        && has_object_array_fields(
               value,
               "named",
               {
                   { "name", JsonValue::value_t::string },
                   { "window", JsonValue::value_t::number_unsigned },
                   { "pending", JsonValue::value_t::boolean },
               }
        )
        && has_typed_array(value, "pool", JsonValue::value_t::number_unsigned);
}

std::optional<std::array<uint32_t, 4>> get_frame_extents(X11Connection& conn, xcb_window_t window, xcb_atom_t atom)
{
    auto cookie = xcb_get_property(conn.get(), 0, window, atom, XCB_ATOM_CARDINAL, 0, 4);
    auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
    if (!reply || reply->type != XCB_ATOM_CARDINAL || reply->format != 32
        || xcb_get_property_value_length(reply) != 4 * static_cast<int>(sizeof(uint32_t)))
    {
        free(reply);
        return std::nullopt;
    }

    auto* values = static_cast<uint32_t*>(xcb_get_property_value(reply));
    std::array<uint32_t, 4> result{ values[0], values[1], values[2], values[3] };
    free(reply);
    return result;
}

bool intersects_root(X11Connection& conn, WindowGeometry const& geometry)
{
    int32_t const left = std::max<int32_t>(0, geometry.x);
    int32_t const top = std::max<int32_t>(0, geometry.y);
    int32_t const right = std::min<int32_t>(conn.screen()->width_in_pixels, geometry.x + geometry.width);
    int32_t const bottom = std::min<int32_t>(conn.screen()->height_in_pixels, geometry.y + geometry.height);
    return left < right && top < bottom;
}

bool window_is_viewable(X11Connection& conn, xcb_window_t window)
{
    auto cookie = xcb_get_window_attributes(conn.get(), window);
    auto* reply = xcb_get_window_attributes_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return false;
    bool const result = reply->map_state == XCB_MAP_STATE_VIEWABLE;
    free(reply);
    return result;
}

std::optional<xcb_window_t> get_input_focus(X11Connection& conn)
{
    auto cookie = xcb_get_input_focus(conn.get());
    auto* reply = xcb_get_input_focus_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return std::nullopt;
    xcb_window_t const result = reply->focus;
    free(reply);
    return result;
}

void synchronize_x_server(X11Connection& conn)
{
    auto cookie = xcb_get_input_focus(conn.get());
    auto* reply = xcb_get_input_focus_reply(conn.get(), cookie, nullptr);
    free(reply);
}

bool drain_window_visibility_events(X11Connection& conn, xcb_window_t window)
{
    bool observed = false;
    while (auto* event = xcb_poll_for_event(conn.get()))
    {
        uint8_t const response_type = event->response_type & 0x7f;
        if (response_type == XCB_MAP_NOTIFY)
        {
            auto* map = reinterpret_cast<xcb_map_notify_event_t*>(event);
            observed = observed || map->window == window;
        }
        else if (response_type == XCB_UNMAP_NOTIFY)
        {
            auto* unmap = reinterpret_cast<xcb_unmap_notify_event_t*>(event);
            observed = observed || unmap->window == window;
        }
        free(event);
    }
    return observed;
}

std::optional<bool>
property_contains_atom(X11Connection& conn, xcb_window_t window, xcb_atom_t property, xcb_atom_t expected)
{
    auto cookie = xcb_get_property(conn.get(), 0, window, property, XCB_ATOM_ATOM, 0, 64);
    auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
    if (!reply)
        return std::nullopt;
    if (reply->type == XCB_ATOM_NONE && reply->format == 0 && xcb_get_property_value_length(reply) == 0)
    {
        free(reply);
        return false;
    }
    if (reply->type != XCB_ATOM_ATOM || reply->format != 32)
    {
        free(reply);
        return std::nullopt;
    }

    bool result = false;
    auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
    int const count = xcb_get_property_value_length(reply) / 4;
    for (int i = 0; i < count; ++i) result = result || atoms[i] == expected;
    free(reply);
    return result;
}

std::optional<std::vector<xcb_window_t>> get_client_list(X11Connection& conn, xcb_atom_t client_list)
{
    auto cookie = xcb_get_property(conn.get(), 0, conn.root(), client_list, XCB_ATOM_WINDOW, 0, 4096);
    auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
    if (!reply || reply->type != XCB_ATOM_WINDOW || reply->format != 32)
    {
        free(reply);
        return std::nullopt;
    }

    int const count = xcb_get_property_value_length(reply) / 4;
    auto* windows = static_cast<xcb_window_t*>(xcb_get_property_value(reply));
    std::vector<xcb_window_t> result(windows, windows + count);
    std::sort(result.begin(), result.end());
    free(reply);
    return result;
}

} // namespace

TEST_CASE("Integration: workspace switch back and forth", "[integration][workspace]")
{
    auto test_env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_number_of_desktops != XCB_NONE);

    uint32_t num_desktops = require_property_cardinal(conn.get(), conn.root(), net_number_of_desktops);
    REQUIRE(num_desktops == 2);
    REQUIRE(require_property_cardinal(conn.get(), conn.root(), net_current_desktop) == 0);

    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    send_client_message(conn, conn.root(), net_current_desktop, 0);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 0, kTimeout));
}

TEST_CASE("Integration: windows persist across workspace switches", "[integration][workspace]")
{
    auto test_env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_number_of_desktops != XCB_NONE);

    uint32_t num_desktops = require_property_cardinal(conn.get(), conn.root(), net_number_of_desktops);
    REQUIRE(num_desktops == 2);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    uint32_t initial_desktop = require_property_cardinal(conn.get(), conn.root(), net_current_desktop);
    uint32_t w1_desktop = require_property_cardinal(conn.get(), w1, net_wm_desktop);
    REQUIRE(w1_desktop == initial_desktop);

    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    w1_desktop = require_property_cardinal(conn.get(), w1, net_wm_desktop);
    REQUIRE(w1_desktop == initial_desktop);

    send_client_message(conn, conn.root(), net_current_desktop, initial_desktop);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, initial_desktop, kTimeout));
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    destroy_window(conn, w1);
}

TEST_CASE(
    "Integration: fullscreen window maintains state across workspace switch",
    "[integration][workspace][fullscreen]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_fullscreen != XCB_NONE);
    REQUIRE(net_number_of_desktops != XCB_NONE);

    uint32_t num_desktops = require_property_cardinal(conn.get(), conn.root(), net_number_of_desktops);
    REQUIRE(num_desktops == 2);

    // Create window and make it fullscreen
    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    // Send _NET_WM_STATE_FULLSCREEN message to toggle fullscreen
    uint32_t values[5] = { 1, net_wm_state_fullscreen, 0, 0, 0 };
    send_client_message(conn, w1, net_wm_state, values[0], values[1], values[2], values[3], values[4]);

    // Verify fullscreen state is set
    auto check_fullscreen = [&]()
    {
        auto cookie = xcb_get_property(conn.get(), 0, w1, net_wm_state, XCB_ATOM_ATOM, 0, 10);
        auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
        if (!reply)
            return false;
        bool has_fullscreen = false;
        xcb_atom_t* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
        int len = xcb_get_property_value_length(reply) / 4;
        for (int i = 0; i < len; i++)
        {
            if (atoms[i] == net_wm_state_fullscreen)
                has_fullscreen = true;
        }
        free(reply);
        return has_fullscreen;
    };

    REQUIRE(wait_for_condition(check_fullscreen, kTimeout));

    // Get initial desktop
    uint32_t initial_desktop = require_property_cardinal(conn.get(), conn.root(), net_current_desktop);

    // Switch to workspace 1
    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));

    // Switch back to workspace 0
    send_client_message(conn, conn.root(), net_current_desktop, initial_desktop);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, initial_desktop, kTimeout));

    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    REQUIRE(check_fullscreen());

    // Exit fullscreen
    values[0] = 0;
    send_client_message(conn, w1, net_wm_state, values[0], values[1], values[2], values[3], values[4]);

    auto check_not_fullscreen = [&]()
    {
        auto cookie = xcb_get_property(conn.get(), 0, w1, net_wm_state, XCB_ATOM_ATOM, 0, 10);
        auto* reply = xcb_get_property_reply(conn.get(), cookie, nullptr);
        if (!reply)
            return false;
        bool has_fullscreen = false;
        xcb_atom_t* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
        int len = xcb_get_property_value_length(reply) / 4;
        for (int i = 0; i < len; i++)
        {
            if (atoms[i] == net_wm_state_fullscreen)
                has_fullscreen = true;
        }
        free(reply);
        return !has_fullscreen;
    };

    REQUIRE(wait_for_condition(check_not_fullscreen, kTimeout));

    destroy_window(conn, w1);
}

// =============================================================================
// Desktop move: moving the focused window to a hidden workspace via
// _NET_WM_DESKTOP should transfer focus to a remaining window.
// =============================================================================
TEST_CASE(
    "Integration: moving focused window to hidden workspace preserves normal mapped state",
    "[integration][workspace][focus][visibility]"
)
{
    auto test_env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;

    xcb_atom_t net_current_desktop = intern_atom(conn.get(), "_NET_CURRENT_DESKTOP");
    xcb_atom_t net_wm_desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    xcb_atom_t net_number_of_desktops = intern_atom(conn.get(), "_NET_NUMBER_OF_DESKTOPS");
    xcb_atom_t net_active_window = intern_atom(conn.get(), "_NET_ACTIVE_WINDOW");
    xcb_atom_t net_client_list = intern_atom(conn.get(), "_NET_CLIENT_LIST");
    xcb_atom_t net_wm_state = intern_atom(conn.get(), "_NET_WM_STATE");
    xcb_atom_t net_wm_state_hidden = intern_atom(conn.get(), "_NET_WM_STATE_HIDDEN");
    xcb_atom_t wm_state = intern_atom(conn.get(), "WM_STATE");
    REQUIRE(net_current_desktop != XCB_NONE);
    REQUIRE(net_wm_desktop != XCB_NONE);
    REQUIRE(net_number_of_desktops != XCB_NONE);
    REQUIRE(net_active_window != XCB_NONE);
    REQUIRE(net_client_list != XCB_NONE);
    REQUIRE(net_wm_state != XCB_NONE);
    REQUIRE(net_wm_state_hidden != XCB_NONE);
    REQUIRE(wm_state != XCB_NONE);

    uint32_t num_desktops = require_property_cardinal(conn.get(), conn.root(), net_number_of_desktops);
    REQUIRE(num_desktops >= 2);

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    uint32_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn.get(), w2, XCB_CW_EVENT_MASK, &event_mask);
    xcb_flush(conn.get());
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto focus = get_input_focus(conn);
            return focus && *focus == w2;
        },
        kTimeout
    ));
    synchronize_x_server(conn);
    drain_window_visibility_events(conn, w2);

    auto client_list_before = get_client_list(conn, net_client_list);
    REQUIRE(client_list_before.has_value());
    REQUIRE(std::binary_search(client_list_before->begin(), client_list_before->end(), w1));
    REQUIRE(std::binary_search(client_list_before->begin(), client_list_before->end(), w2));
    REQUIRE(get_wm_state(conn, w2, wm_state) == XCB_ICCCM_WM_STATE_NORMAL);

    // Move w2 to desktop 1 while desktop 0 remains current.
    send_client_message(conn, w2, net_wm_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), w2, net_wm_desktop, 1, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto current = get_window_property_cardinal(conn.get(), conn.root(), net_current_desktop);
            auto geometry = get_window_geometry(conn, w2);
            return current && *current == 0 && geometry && !intersects_root(conn, *geometry);
        },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto hidden = property_contains_atom(conn, w2, net_wm_state, net_wm_state_hidden);
            auto state = get_wm_state(conn, w2, wm_state);
            auto client_list = get_client_list(conn, net_client_list);
            auto active = get_window_property_window(conn.get(), conn.root(), net_active_window);
            auto focus = get_input_focus(conn);
            return window_is_viewable(conn, w2) && state && *state == XCB_ICCCM_WM_STATE_NORMAL && hidden && !*hidden
                && client_list && *client_list == *client_list_before && active && *active == w1 && focus
                && *focus == w1;
        },
        kTimeout
    ));
    synchronize_x_server(conn);
    CHECK_FALSE(drain_window_visibility_events(conn, w2));

    send_client_message(conn, conn.root(), net_current_desktop, 1);
    REQUIRE(wait_for_property_cardinal(conn.get(), conn.root(), net_current_desktop, 1, kTimeout));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto geometry = get_window_geometry(conn, w2);
            return geometry && intersects_root(conn, *geometry);
        },
        kTimeout
    ));
    REQUIRE(wait_for_condition(
        [&]()
        {
            auto hidden = property_contains_atom(conn, w2, net_wm_state, net_wm_state_hidden);
            auto state = get_wm_state(conn, w2, wm_state);
            auto client_list = get_client_list(conn, net_client_list);
            auto active = get_window_property_window(conn.get(), conn.root(), net_active_window);
            auto focus = get_input_focus(conn);
            return window_is_viewable(conn, w2) && state && *state == XCB_ICCCM_WM_STATE_NORMAL && hidden && !*hidden
                && client_list && *client_list == *client_list_before && active && *active == w2 && focus
                && *focus == w2;
        },
        kTimeout
    ));
    synchronize_x_server(conn);
    CHECK_FALSE(drain_window_visibility_events(conn, w2));

    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

// =============================================================================
// Monocle layout: every tiled window should occupy the same content rect after
// `lwmctl layout set monocle`. Stacking decides which is visible on top.
// =============================================================================
TEST_CASE("Integration: monocle layout assigns identical geometries", "[integration][layout][monocle]")
{
    auto test_env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto socket_path = wait_for_ipc_socket_path(conn);
    REQUIRE(socket_path.has_value());

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    xcb_window_t w3 = create_window(conn, 70, 70, 200, 150);
    map_window(conn, w3);
    REQUIRE(wait_for_active_window(conn, w3, kTimeout));

    auto g1 = get_window_geometry(conn, w1);
    auto g2 = get_window_geometry(conn, w2);
    auto g3 = get_window_geometry(conn, w3);
    REQUIRE(g1.has_value());
    REQUIRE(g2.has_value());
    REQUIRE(g3.has_value());
    bool all_equal_master_stack = (*g1 == *g2) && (*g2 == *g3);
    REQUIRE_FALSE(all_equal_master_stack);

    auto result = run_lwmctl(test_env->wm, { "layout", "set", "monocle" }, *socket_path);
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);

    bool ok = wait_for_condition(
        [&]()
        {
            auto a = get_window_geometry(conn, w1);
            auto b = get_window_geometry(conn, w2);
            auto c = get_window_geometry(conn, w3);
            return a && b && c && *a == *b && *b == *c;
        },
        kTimeout
    );
    REQUIRE(ok);

    auto restore = run_lwmctl(test_env->wm, { "layout", "set", "master-stack" }, *socket_path);
    REQUIRE(restore.has_value());
    REQUIRE(restore->exit_code == 0);

    destroy_window(conn, w3);
    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: JSON list IPC replies match the documented schema", "[integration][ipc][json]")
{
    auto test_env = TestEnvironment::create(R"(
[commands]
terminal = { argv = ["/bin/true"] }

[workspaces]
count = 2

[[scratchpads]]
name = "terminal"
spawn = { ref = "terminal" }
match = { class = "ScratchpadClass" }
)");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto socket_path = wait_for_ipc_socket_path(conn);
    REQUIRE(socket_path.has_value());

    xcb_window_t window = create_window(conn, 10, 10, 200, 150);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));

    auto stash = send_raw_ipc(*socket_path, "scratchpad stash");
    REQUIRE(stash.has_value());
    REQUIRE(stash->starts_with("ok"));

    auto workspaces = send_raw_ipc(*socket_path, "workspace list");
    REQUIRE(workspaces.has_value());
    auto workspace_json = parse_ok_json(*workspaces);
    REQUIRE(workspace_json.has_value());
    REQUIRE(workspace_list_has_documented_shape(*workspace_json));

    auto windows = send_raw_ipc(*socket_path, "window list");
    REQUIRE(windows.has_value());
    auto window_json = parse_ok_json(*windows);
    REQUIRE(window_json.has_value());
    REQUIRE(window_list_has_documented_shape(*window_json));

    auto scratchpads = send_raw_ipc(*socket_path, "scratchpad list");
    REQUIRE(scratchpads.has_value());
    auto scratchpad_json = parse_ok_json(*scratchpads);
    REQUIRE(scratchpad_json.has_value());
    REQUIRE(scratchpad_list_has_documented_shape(*scratchpad_json));

    destroy_window(conn, window);
}

TEST_CASE("Integration: managed windows publish zero frame extents", "[integration][ewmh][frame_extents]")
{
    auto test_env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    xcb_atom_t frame_extents = intern_atom(conn.get(), "_NET_FRAME_EXTENTS");
    REQUIRE(frame_extents != XCB_NONE);

    xcb_window_t window = create_window(conn, 10, 10, 200, 150);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    REQUIRE(wait_for_condition([&]() { return get_frame_extents(conn, window, frame_extents).has_value(); }, kTimeout));

    auto extents = get_frame_extents(conn, window, frame_extents);
    REQUIRE(extents.has_value());
    CHECK(*extents == std::array<uint32_t, 4>{ 0, 0, 0, 0 });

    destroy_window(conn, window);
}

TEST_CASE("Integration: monocle swap focuses adjacent tiled window", "[integration][layout][monocle][swap]")
{
    auto test_env = TestEnvironment::create(R"(
[workspaces]
count = 2

[[binds]]
key = "super+j"
swap_next = true
)");
    if (!test_env)
        SKIP("Test environment not available");

    auto& conn = test_env->conn;
    auto socket_path = wait_for_ipc_socket_path(conn);
    REQUIRE(socket_path.has_value());

    xcb_window_t w1 = create_window(conn, 10, 10, 200, 150);
    map_window(conn, w1);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));
    xcb_window_t w2 = create_window(conn, 40, 40, 200, 150);
    map_window(conn, w2);
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));
    xcb_window_t w3 = create_window(conn, 70, 70, 200, 150);
    map_window(conn, w3);
    REQUIRE(wait_for_active_window(conn, w3, kTimeout));

    xcb_atom_t dialog = intern_atom(conn.get(), "_NET_WM_WINDOW_TYPE_DIALOG");
    REQUIRE(dialog != XCB_NONE);
    xcb_window_t floating = create_window(conn, 100, 100, 180, 120);
    set_window_type(conn, floating, dialog);
    map_window(conn, floating);
    REQUIRE(wait_for_active_window(conn, floating, kTimeout));

    auto set_layout = run_lwmctl(test_env->wm, { "layout", "set", "monocle" }, *socket_path);
    REQUIRE(set_layout.has_value());
    REQUIRE(set_layout->exit_code == 0);

    auto focus = run_lwmctl(test_env->wm, { "focus", "window=" + std::to_string(w1) }, *socket_path);
    REQUIRE(focus.has_value());
    REQUIRE(focus->exit_code == 0);
    REQUIRE(wait_for_active_window(conn, w1, kTimeout));

    REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("j")));
    REQUIRE(wait_for_active_window(conn, w2, kTimeout));

    destroy_window(conn, floating);
    destroy_window(conn, w3);
    destroy_window(conn, w2);
    destroy_window(conn, w1);
}

TEST_CASE("Integration: workspace, fullscreen, scratchpad and restart transitions compose", "[integration][sequence][restart]")
{
    auto env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!env)
        SKIP("Xvfb not available");
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto command = [&](std::string const& text)
    {
        auto response = send_ipc_command(*socket, text);
        REQUIRE(response);
        REQUIRE(response->starts_with("ok"));
    };
    xcb_window_t a = create_window(conn, 20, 20, 200, 200);
    xcb_window_t b = create_window(conn, 40, 40, 200, 200);
    map_window(conn, a);
    REQUIRE(wait_for_active_window(conn, a, kTimeout));
    map_window(conn, b);
    REQUIRE(wait_for_active_window(conn, b, kTimeout));
    auto state = intern_atom(conn.get(), "_NET_WM_STATE");
    auto fullscreen = intern_atom(conn.get(), "_NET_WM_STATE_FULLSCREEN");
    auto desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    auto visible = [&](xcb_window_t w)
    {
        auto geometry = get_window_geometry(conn, w);
        return geometry && geometry->x >= 0;
    };
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        CAPTURE(iteration);
        command("focus window=" + std::to_string(b));
        send_client_message(conn, b, state, 1, fullscreen);
        REQUIRE(wait_for_condition([&] { return visible(b) && !visible(a); }, kTimeout));
        // Moving the owner away restores the old workspace; activating it again
        // resolves destination visibility before committing focus.
        send_client_message(conn, b, desktop, 1);
        REQUIRE(wait_for_condition([&] { return visible(a) && !visible(b); }, kTimeout));
        REQUIRE(wait_for_active_window(conn, a, kTimeout));
        command("focus window=" + std::to_string(b));
        REQUIRE(wait_for_active_window(conn, b, kTimeout));
        REQUIRE(wait_for_condition([&] { return visible(b) && !visible(a); }, kTimeout));
        send_client_message(conn, b, state, 0, fullscreen);
        // Wait for the state request before issuing a command on another socket.
        REQUIRE(wait_for_condition(
            [&]
            {
                auto geometry = get_window_geometry(conn, b);
                return geometry && geometry->x > 0;
            },
            kTimeout
        ));
        command(iteration == 0 ? "scratchpad stash" : "scratchpad cycle");
        REQUIRE(wait_for_condition([&] { return !visible(b); }, kTimeout));
        command("workspace switch 0");
        REQUIRE(wait_for_active_window(conn, a, kTimeout));
        command("scratchpad cycle");
        REQUIRE(wait_for_active_window(conn, b, kTimeout));
        REQUIRE(wait_for_condition([&] { return visible(a) && visible(b); }, kTimeout));
        auto previous = wm_instance(conn);
        REQUIRE(previous);
        command("restart");
        REQUIRE(wait_for_wm_restart(conn, kTimeout, *previous));
        REQUIRE(wait_for_active_window(conn, b, kTimeout));
        REQUIRE(wait_for_condition([&] { return visible(a) && visible(b); }, kTimeout));
    }
    destroy_window(conn, b);
    REQUIRE(wait_for_active_window(conn, a, kTimeout));
    destroy_window(conn, a);
    REQUIRE(wait_for_active_window(conn, XCB_NONE, kTimeout));
}

TEST_CASE("Integration: invalid ratio commands cannot poison layout state", "[integration][layout][ipc]")
{
    auto env = TestEnvironment::create();
    if (!env)
        SKIP("Xvfb not available");
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    auto window = create_window(conn, 20, 20, 200, 200);
    map_window(conn, window);
    REQUIRE(wait_for_active_window(conn, window, kTimeout));
    auto geometry = get_window_geometry(conn, window);
    REQUIRE(geometry);
    for (std::string action : { "set", "adjust" })
        for (std::string value : { "nan", "inf", "-inf", "0.5junk", "1e9999" })
        {
            CAPTURE(action, value);
            auto response = send_ipc_command(*socket, "ratio " + action + " " + value);
            REQUIRE(response);
            CHECK(response->starts_with("error "));
        }
    CHECK(send_ipc_command(*socket, "ping") == "ok pong");
    CHECK(get_window_geometry(conn, window) == geometry);
    destroy_window(conn, window);
}

TEST_CASE("Integration: restart consumes its handoff and rejects damaged snapshots", "[integration][restart][malformed][layout][monocle]")
{
    auto env = TestEnvironment::create("[workspaces]\ncount = 2\n");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    ipc_ok(*socket, "workspace switch 1");
    auto first = create_window(conn, 10, 10, 200, 150);
    auto second = create_window(conn, 40, 40, 200, 150);
    for (auto window : { first, second })
    {
        map_window(conn, window);
        REQUIRE(wait_for_active_window(conn, window, kTimeout));
    }
    ipc_ok(*socket, "layout set monocle");
    REQUIRE(require_window_geometry(conn, first) == require_window_geometry(conn, second));
    auto instance = wm_instance(conn);
    REQUIRE(instance);
    PausedRestart restart(env->wm, *socket);
    auto property = intern_atom(conn.get(), "_LWM_RESTART");
    auto snapshot = read_property32(conn.get(), conn.root(), property, XCB_ATOM_CARDINAL);
    REQUIRE(snapshot);
    REQUIRE(snapshot->size() > 1);
    bool intact = false;
    SECTION("Intact handoff preserves layout") { intact = true; }
    SECTION("Incompatible format") { ++snapshot->front(); }
    SECTION("Current format with a truncated record") { snapshot->pop_back(); }
    SECTION("Current format with trailing data") { snapshot->push_back(0); }
    xcb_change_property(
        conn.get(),
        XCB_PROP_MODE_REPLACE,
        conn.root(),
        property,
        XCB_ATOM_CARDINAL,
        32,
        static_cast<uint32_t>(snapshot->size()),
        snapshot->data()
    );
    REQUIRE(get_window_geometry(conn, conn.root())); // Finish the edit before resuming startup.
    restart.resume();
    REQUIRE(wait_for_wm_restart(conn, kTimeout, *instance));
    CHECK_FALSE(read_property32(conn.get(), conn.root(), property, XCB_ATOM_CARDINAL));
    auto workspaces = ipc_json(*socket, "workspace list");
    CHECK(
        workspaces.at("monitors").at(0).at("workspaces").at(1).at("layout")
        == (intact ? "monocle" : "master-stack")
    );
    // A rejected handoff still adopts both clients on their EWMH desktop and
    // arranges them afresh; it must neither lose them nor apply partial state.
    ipc_ok(*socket, "workspace switch 1");
    auto desktop = intern_atom(conn.get(), "_NET_WM_DESKTOP");
    for (auto window : { first, second })
    {
        CHECK(require_property_cardinal(conn.get(), window, desktop) == 1);
        CHECK(get_window_border_width(conn, window) == 2);
        CHECK_FALSE(is_hidden_offscreen(conn, window));
    }
    CHECK((require_window_geometry(conn, first) == require_window_geometry(conn, second)) == intact);
    destroy_window(conn, second);
    destroy_window(conn, first);
}

TEST_CASE("Integration: restart places unrecorded windows on the restored workspace", "[integration][restart]")
{
    auto env = TestEnvironment::create("[workspaces]\ncount = 3\n");
    if (!env)
        SKIP("Test environment not available");
    auto& conn = env->conn;
    auto path = wait_for_ipc_socket_path(conn);
    REQUIRE(path);
    REQUIRE(send_ipc_command(*path, "workspace switch 2")->starts_with("ok"));
    // A viewable window the old WM never managed, like one mapped during the exec gap.
    auto window = create_window(conn, 10, 10, 200, 150);
    uint32_t override_redirect = 1;
    xcb_change_window_attributes(conn.get(), window, XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
    map_window(conn, window);
    override_redirect = 0;
    xcb_change_window_attributes(conn.get(), window, XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
    xcb_flush(conn.get());
    auto previous = wm_instance(conn);
    REQUIRE(previous);
    REQUIRE(send_ipc_command(*path, "restart"));
    REQUIRE(wait_for_wm_restart(conn, std::chrono::seconds(5), *previous));
    REQUIRE(wait_for_condition(
        [&]
        {
            auto reply = send_ipc_command(*path, "window list");
            return reply && reply->find("\"id\":" + std::to_string(window) + ",\"monitor\":0,\"workspace\":2")
                != std::string::npos;
        },
        kTimeout
    ));
    auto geometry = get_window_geometry(conn, window);
    REQUIRE(geometry);
    CHECK(geometry->x >= 0);
    destroy_window(conn, window);
}

TEST_CASE("Integration: restart preserves the selected workspace and previous-workspace key binding", "[integration][restart][workspace][keybind]")
{
    auto env = TestEnvironment::create(R"(
[workspaces]
count = 4
[[binds]]
key = "super+Tab"
toggle_workspace = true
[[binds]]
key = "super+r"
restart = true
)");
    if (!env)
        SKIP("X11 unavailable");
    auto& conn = env->conn;
    auto socket = wait_for_ipc_socket_path(conn);
    REQUIRE(socket);
    bool populated = false;
    SECTION("Empty workspaces") { }
    SECTION("Workspaces with clients") { populated = true; }
    std::vector<xcb_window_t> windows;
    for (size_t workspace : { 1, 2 })
    {
        ipc_ok(*socket, "workspace switch " + std::to_string(workspace));
        if (populated)
        {
            auto window = create_window(conn, 20, 20, 200, 150);
            windows.push_back(window);
            map_window(conn, window);
            REQUIRE(wait_for_active_window(conn, window, kTimeout));
        }
    }
    auto current = [&] { return ipc_json(*socket, "workspace list").at("monitors").at(0).at("current_workspace"); };
    for (int iteration = 0; iteration < 2; ++iteration)
    {
        CAPTURE(iteration, populated);
        auto instance = wm_instance(conn);
        REQUIRE(instance);
        REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("r")));
        REQUIRE(wait_for_wm_restart(conn, kTimeout, *instance));
        CHECK(current() == 2);
        if (populated)
            CHECK(wait_for_active_window(conn, windows[1], kTimeout));
        REQUIRE(send_key_chord(conn, XStringToKeysym("Super_L"), XStringToKeysym("Tab")));
        REQUIRE(wait_for_condition([&] { return current() == 1; }, kTimeout));
        if (populated)
            CHECK(wait_for_active_window(conn, windows[0], kTimeout));
        ipc_ok(*socket, "workspace toggle");
        CHECK(current() == 2);
    }
    for (auto window : windows) destroy_window(conn, window);
}

TEST_CASE("Integration: startup replaces client lists left by a previous manager", "[integration][ewmh]")
{
    auto& x11 = X11TestEnvironment::instance();
    if (!x11.available())
        SKIP("X11 unavailable");
    X11Connection conn;
    REQUIRE(conn.ok());
    xcb_window_t stale = 0x1234567;
    for (auto name : { "_NET_CLIENT_LIST", "_NET_CLIENT_LIST_STACKING" })
        xcb_change_property(
            conn.get(),
            XCB_PROP_MODE_REPLACE,
            conn.root(),
            intern_atom(conn.get(), name),
            XCB_ATOM_WINDOW,
            32,
            1,
            &stale
        );
    xcb_flush(conn.get());
    LwmProcess wm(x11.display(), "");
    REQUIRE(wm.running());
    REQUIRE(wait_for_wm_ready(conn, kTimeout));
    for (auto name : { "_NET_CLIENT_LIST", "_NET_CLIENT_LIST_STACKING" })
    {
        CAPTURE(name);
        auto* reply = xcb_get_property_reply(
            conn.get(),
            xcb_get_property(conn.get(), 0, conn.root(), intern_atom(conn.get(), name), XCB_ATOM_WINDOW, 0, 16),
            nullptr
        );
        REQUIRE(reply);
        CHECK(xcb_get_property_value_length(reply) == 0);
        free(reply);
    }
}
