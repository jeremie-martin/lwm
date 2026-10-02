#include "lwm/core/overloaded.hpp"
#include "lwm/core/log.hpp"
#include "wm.hpp"
#include <algorithm>

namespace lwm {

namespace {

std::string ok(std::string const& value) { return value.empty() ? "ok" : "ok " + value; }

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
                        return ok(workspace_list_json());
                    case command::Query::WindowList:
                        return ok(window_list_json());
                    case command::Query::ScratchpadList:
                        return ok(scratchpad_list_json());
                    case command::Query::State:
                        return ok(
                            "{\"instance\":\"" + ipc_.instance() + "\",\"sequence\":" + std::to_string(ipc_.sequence())
                            + "," + state_json().substr(1)
                        );
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

std::string WindowManager::workspace_list_json() const
{
    auto const& monitors = state_.monitors();
    std::string json = "{\"focused_monitor\":" + std::to_string(state_.focused_monitor()) + ",\"monitors\":[";
    for (size_t m = 0; m < monitors.size(); ++m)
    {
        auto const& monitor = monitors[m];
        json += std::string(m ? "," : "") + "{\"index\":" + std::to_string(m) + ",\"name\":" + json_string(monitor.name)
            + ",\"current_workspace\":" + std::to_string(monitor.current_workspace) + ",\"workspaces\":[";
        for (size_t w = 0; w < monitor.workspaces.size(); ++w)
        {
            auto const& workspace = monitor.workspaces[w];
            json += std::string(w ? "," : "") + "{\"index\":" + std::to_string(w) + ",\"name\":"
                + json_string(config_.workspaces.names[w]) + ",\"current\":" + json_bool(w == monitor.current_workspace)
                + ",\"window_count\":" + std::to_string(workspace.windows.size()) + ",\"layout\":"
                + json_string(layout_strategy_str(workspace.layout_strategy)) + "}";
        }
        json += "]}";
    }
    return json + "]}";
}

std::string WindowManager::window_list_json() const
{
    auto clients = state_.clients_by_order();
    std::string json = "{\"focused\":" + std::to_string(state_.active_window()) + ",\"windows\":[";
    for (size_t i = 0; i < clients.size(); ++i)
    {
        auto const& c = *clients[i];
        json += std::string(i ? "," : "") + "{\"id\":" + std::to_string(c.id) + ",\"monitor\":" + std::to_string(c.monitor)
            + ",\"workspace\":" + std::to_string(c.workspace) + ",\"kind\":" + json_string(client_kind_str(c.kind()))
            + ",\"class\":" + json_string(c.wm_class) + ",\"instance\":" + json_string(c.wm_class_name)
            + ",\"title\":" + json_string(c.name) + ",\"focused\":" + json_bool(c.id == state_.active_window())
            + ",\"fullscreen\":" + json_bool(c.fullscreen) + ",\"urgent\":" + json_bool(c.urgency.active())
            + ",\"sticky\":" + json_bool(c.sticky) + ",\"iconic\":" + json_bool(c.iconic) + "}";
    }
    return json + "]}";
}

std::string WindowManager::scratchpad_list_json() const
{
    std::string json = "{\"named\":[";
    auto const& named = state_.named_scratchpads();
    for (size_t i = 0; i < named.size(); ++i)
        json += std::string(i ? "," : "") + "{\"name\":" + json_string(named[i].name)
            + ",\"window\":" + std::to_string(named[i].window()) + ",\"pending\":" + json_bool(named[i].pending_launch()) + "}";
    json += "],\"pool\":[";
    auto const& pool = state_.scratchpad_pool();
    for (size_t i = 0; i < pool.size(); ++i) json += std::string(i ? "," : "") + std::to_string(pool[i]);
    return json + "]}";
}

// The exposed state; state_change fires exactly when it differs.
std::string WindowManager::state_json() const
{
    return "{\"workspaces\":" + workspace_list_json() + ",\"windows\":" + window_list_json()
        + ",\"scratchpads\":" + scratchpad_list_json() + "}";
}

} // namespace lwm
