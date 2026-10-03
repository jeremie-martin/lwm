#include "lwm/core/overloaded.hpp"
#include "lwm/core/log.hpp"
#include "wm.hpp"
#include <algorithm>
#include <rfl/Flatten.hpp>
#include <rfl/Rename.hpp>
#include <rfl/json/write.hpp>

namespace lwm {

namespace {

std::string ok(std::string const& value) { return value.empty() ? "ok" : "ok " + value; }

// Query records are the IPC wire schema; field order is the JSON order.
struct WorkspaceView
{
    size_t index;
    std::string_view name;
    bool current;
    size_t window_count;
    std::string_view layout;
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
struct StateQuery
{
    std::string_view instance;
    uint64_t sequence;
    rfl::Flatten<StateView const*> state;
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
            view.workspaces.push_back({ w,
                                        state.config().workspaces.names[w],
                                        w == monitor.current_workspace,
                                        workspace.windows.size(),
                                        layout_strategy_str(workspace.layout_strategy) });
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
                                 client_kind_str(c->kind()),
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

std::string WindowManager::handle_request(command::Request const& request)
{
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
                    {
                        StateView view{ workspace_list(state_), window_list(state_), scratchpad_list(state_) };
                        return ok(json(StateQuery{ ipc_.instance(), ipc_.sequence(), &view }));
                    }
                }
                return "error unknown query";
            },
            // The server answers subscriptions itself.
            [&](command::Subscribe const&) -> std::string { return "error unexpected subscription"; },
            [&](Action const& action) -> std::string
            {
                auto result = execute(action, "ipc");
                return result ? ok(*result) : "error " + result.error();
            },
        },
        request
    );
}


// The exposed state; state_change fires exactly when it differs.
std::string WindowManager::state_json() const
{
    return json(StateView{ workspace_list(state_), window_list(state_), scratchpad_list(state_) });
}

} // namespace lwm
