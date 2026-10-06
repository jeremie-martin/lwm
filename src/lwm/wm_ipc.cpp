#include "lwm/core/overloaded.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/xproperty.hpp"
#include "wm.hpp"
#include <algorithm>
#include <rfl/Rename.hpp>
#include <rfl/json/write.hpp>

namespace lwm {

namespace {

std::string ok(std::string const& value) { return "ok " + value; }

// Query records are the IPC wire schema; field order is the JSON order.
struct WorkspaceView
{
    size_t index;
    std::string_view name;
    bool current;
    size_t window_count;
    std::string_view layout;
    double ratio; ///< Root split ratio
};
struct MonitorView
{
    size_t index;
    std::string_view name;
    size_t current_workspace;
    std::vector<WorkspaceView> workspaces;
};
struct WorkspaceList
{
    size_t focused_monitor;
    std::vector<MonitorView> monitors;
};
struct WindowView
{
    uint32_t id;
    size_t monitor, workspace;
    std::string_view kind;
    rfl::Rename<"class", std::string_view> wm_class;
    std::string_view instance, title;
    bool focused, fullscreen, urgent, sticky, iconic;
};
struct WindowList
{
    uint32_t focused;
    std::vector<WindowView> windows;
};
struct NamedView
{
    std::string_view name;
    uint32_t window;
    bool pending;
};
struct ScratchpadList
{
    std::vector<NamedView> named;
    std::vector<xcb_window_t> pool;
};
struct StateView
{
    WorkspaceList workspaces;
    WindowList windows;
    ScratchpadList scratchpads;
};
// X metadata can contain opaque bytes; preserve them.
template <typename T> std::string json(T const& value) { return rfl::json::write(value, YYJSON_WRITE_ALLOW_INVALID_UNICODE); }

WorkspaceList workspace_list(State const& state)
{
    WorkspaceList list{ state.focused_monitor(), { } };
    auto const& monitors = state.monitors();
    for (size_t m = 0; m < monitors.size(); ++m)
    {
        auto const& monitor = monitors[m];
        auto& view = list.monitors.emplace_back(MonitorView{ m, monitor.name, monitor.current_workspace, { } });
        for (size_t w = 0; w < monitor.workspaces.size(); ++w)
        {
            auto const& workspace = monitor.workspaces[w];
            auto root = workspace.split_ratios.find(SplitAddress{ 0 });
            view.workspaces.push_back({ w,
                                        state.config().workspaces[w],
                                        w == monitor.current_workspace,
                                        workspace.windows.size(),
                                        layout_strategy_str(workspace.layout_strategy),
                                        root == workspace.split_ratios.end() ? state.config().layout.default_ratio : root->second });
        }
    }
    return list;
}

WindowList window_list(State const& state)
{
    WindowList list{ state.active_window(), { } };
    for (auto const* c : state.clients_by_order())
        list.windows.push_back({ c->id,
                                 c->monitor,
                                 c->workspace,
                                 client_kind_str(*c),
                                 c->wm_class,
                                 c->wm_class_name,
                                 c->name,
                                 c->id == state.active_window(),
                                 c->fullscreen,
                                 c->urgency.active(),
                                 c->sticky,
                                 c->iconic });
    return list;
}

ScratchpadList scratchpad_list(State const& state)
{
    ScratchpadList list{ { }, state.scratchpad_pool() };
    for (auto const& slot : state.named_scratchpads())
        list.named.push_back({ slot.name, slot.claimed_window(), slot.pending_launch() });
    return list;
}

} // namespace

// A requester window carries one command in _LWM_COMMAND. Read-and-delete keeps
// the WM stateless per caller; the reply follows completion on this connection,
// so the X server applies every effect before the caller can observe the reply.
void WindowManager::handle_command(xcb_window_t requester)
{
    auto* c = conn_.get();
    auto utf8 = ewmh_.get()->UTF8_STRING;
    auto request = reply(xcb_get_property_reply(
        c, xcb_get_property(c, 1, requester, atoms_.lwm_command, utf8, 0, command::max_request_bytes / 4), nullptr
    ));
    if (!request || request->type == XCB_NONE)
        return; // Not a request: the property is missing or the window is gone.
    bool stopping = stop_.has_value();
    std::string response = request->type != utf8 || request->format != 8 ? "error request must be UTF-8 text"
        : request->bytes_after                                    ? "error request too large"
        : handle_request({ static_cast<char const*>(xcb_get_property_value(request.get())),
                           static_cast<size_t>(xcb_get_property_value_length(request.get())) });
    complete_transition();
    if (!conn_.fits_property(response.size()))
        response = "error response too large";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, requester, atoms_.lwm_reply, utf8, 8, response.size(), response.data());
    if (!stopping && stop_ == RunResult::Restart)
        restart_requester_ = requester;
}

std::string WindowManager::handle_request(std::string_view text)
{
    auto request = command::parse_command(text);
    if (!request)
        return "error " + request.error();
    return std::visit(
        Overloaded{
            [&](command::Query query) -> std::string
            {
                switch (query)
                {
                    case command::Query::Ping:
                        return ok("pong");
                    case command::Query::Version:
                        return ok(LWM_VERSION);
                    case command::Query::LogStatus:
                        return ok(log::status_json());
                    case command::Query::WorkspaceList:
                        return ok(json(workspace_list(state_)));
                    case command::Query::WindowList:
                        return ok(json(window_list(state_)));
                    case command::Query::ScratchpadList:
                        return ok(json(scratchpad_list(state_)));
                    case command::Query::State:
                        return ok(state_json());
                }
                return "error unknown query";
            },
            [&](Action const& action) -> std::string
            {
                auto result = execute(action, "ipc");
                return result ? "ok" : "error " + result.error();
            },
        },
        *request
    );
}


// The exposed state, also published as _LWM_STATE for watchers.
std::string WindowManager::state_json() const
{
    return json(StateView{ workspace_list(state_), window_list(state_), scratchpad_list(state_) });
}

} // namespace lwm
