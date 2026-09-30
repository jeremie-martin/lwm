#include "wm.hpp"
#include <algorithm>

namespace lwm {
namespace {
std::string ok_reply(std::string const& message)
{
    if (message.empty())
        return "ok";
    return "ok " + message;
}

std::string error_reply(std::string const& message) { return "error " + message; }
}

void WindowManager::queue_event(EventType type, std::string json)
{
    effects_.events.emplace_back(type, std::move(json));
}

std::string WindowManager::run_ipc_command(ipc::Command const& command)
{
    using enum ipc::CommandId;
    if (command.id != Ping && command.id != Version && command.id != WorkspaceList && command.id != WindowList
        && command.id != ScratchpadList && command.id != State && command.id != LogStatus)
        effects_.state_changed |= ipc_.has_subscribers(Event_StateChange);
    if (command.id == LogStatus)
        return ok_reply(log::status_json());

    if (command.id == Ping)
        return ok_reply("pong");

    if (command.id == Version)
        return ok_reply(LWM_VERSION);

    if (command.id == Reload)
    {
        auto result = reload_config();
        emit_config_reload_result(result, "ipc");
        if (!result)
            return error_reply(result.error());
        return ok_reply("reloaded");
    }

    if (command.id == Restart)
    {
        initiate_restart();
        return ok_reply("restarting");
    }

    if (command.id == Exec)
    {
        std::string binary = std::get<std::string>(command.argument);
        initiate_restart(std::move(binary));
        return ok_reply("restarting");
    }

    // Layout commands
    if (command.id == Layout)
    {
        auto const& name = std::get<std::string>(command.argument);
        if (focused_monitor_ >= monitors_.size())
            return error_reply("no focused monitor");
        auto strategy = parse_layout_strategy(name);
        if (!strategy)
            return error_reply("unknown layout: " + name);
        state_.layout(focused_monitor_, *strategy);
        effects_.drain_crossing = true;
        std::string strategy_name = layout_strategy_str(*strategy);
        if (ipc_.has_subscribers(Event_LayoutChange))
        {
            queue_event(
                Event_LayoutChange,
                "{\"event\":\"layout_change\",\"action\":\"layout_set\",\"value\":\"" + strategy_name + "\"}"
            );
        }
        return ok_reply("layout set to " + strategy_name);
    }

    if (command.id == RatioSet)
    {
        double val = std::get<double>(command.argument);
        double min_r = config_.layout.min_ratio;
        if (val < min_r || val > 1.0 - min_r)
            return error_reply(
                "ratio out of range [" + std::to_string(min_r) + ", " + std::to_string(1.0 - min_r) + "]"
            );
        if (focused_monitor_ >= monitors_.size())
            return error_reply("no focused monitor");
        state_.ratio(focused_monitor_, SplitAddress{ 0 }, val);
        if (ipc_.has_subscribers(Event_LayoutChange))
        {
            queue_event(
                Event_LayoutChange,
                "{\"event\":\"layout_change\",\"action\":\"ratio_set\",\"value\":" + std::to_string(val) + "}"
            );
        }
        return ok_reply("ratio set");
    }

    if (command.id == RatioReset)
    {
        if (focused_monitor_ >= monitors_.size())
            return error_reply("no focused monitor");
        state_.reset_ratios(focused_monitor_);
        if (ipc_.has_subscribers(Event_LayoutChange))
        {
            queue_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"ratio_reset\"}");
        }
        return ok_reply("ratios reset");
    }

    if (command.id == RatioAdjust)
    {
        double delta = std::get<double>(command.argument);
        if (focused_monitor_ >= monitors_.size())
            return error_reply("no focused monitor");
        if (!adjust_master_ratio(delta))
            return ok_reply("ratio unchanged");
        if (ipc_.has_subscribers(Event_LayoutChange))
        {
            queue_event(
                Event_LayoutChange,
                "{\"event\":\"layout_change\",\"action\":\"ratio_adjust\",\"delta\":" + std::to_string(delta) + "}"
            );
        }
        return ok_reply("ratio adjusted");
    }

    if (command.id == Attention)
        return handle_notification_attention(std::get<uint32_t>(command.argument));

    if (command.id == WorkspaceSwitch)
    {
        size_t target = std::get<uint32_t>(command.argument);
        if (focused_monitor_ >= monitors_.size())
            return error_reply("no focused monitor");
        if (target >= focused_monitor().workspaces.size())
            return error_reply("workspace out of range");

        switch_workspace(target);
        return ok_reply(std::to_string(target));
    }

    if (command.id == WorkspaceNext || command.id == WorkspacePrev)
    {
        if (focused_monitor_ >= monitors_.size())
            return error_reply("no focused monitor");
        size_t count = focused_monitor().workspaces.size();
        if (count == 0)
            return error_reply("no workspaces");
        size_t current = focused_monitor().current_workspace;
        size_t target = (command.id == WorkspaceNext) ? (current + 1) % count : (current + count - 1) % count;
        switch_workspace(target);
        return ok_reply(std::to_string(target));
    }

    if (command.id == WorkspaceList)
    {
        std::string json = "{\"focused_monitor\":" + std::to_string(focused_monitor_) + ",\"monitors\":[";
        for (size_t m = 0; m < monitors_.size(); ++m)
        {
            auto const& monitor = monitors_[m];
            if (m > 0)
                json += ",";
            json += "{\"index\":" + std::to_string(m) + ",\"name\":\"" + json_escape(monitor.name) + "\""
                + ",\"current_workspace\":" + std::to_string(monitor.current_workspace) + ",\"workspaces\":[";
            for (size_t w = 0; w < monitor.workspaces.size(); ++w)
            {
                auto const& ws = monitor.workspaces[w];
                if (w > 0)
                    json += ",";
                json += "{\"index\":" + std::to_string(w) + ",\"name\":\""
                    + json_escape(config_.workspaces.display_name(w)) + "\""
                    + ",\"current\":" + (w == monitor.current_workspace ? "true" : "false")
                    + ",\"window_count\":" + std::to_string(ws.windows.size()) + ",\"layout\":\""
                    + layout_strategy_str(ws.layout_strategy) + "\"}";
            }
            json += "]}";
        }
        json += "]}";
        return ok_reply(json);
    }

    if (command.id == FocusNext || command.id == FocusPrev)
    {
        if (focused_monitor_ >= monitors_.size())
            return error_reply("no focused monitor");
        if (!cycle_focus(command.id == FocusNext))
            return error_reply("no focus candidates");
        return ok_reply(std::to_string(active_window_));
    }

    if (command.id == FocusWindow)
    {
        auto window = std::get<uint32_t>(command.argument);
        auto* client = get_client(window);
        if (!client)
            return error_reply("unknown window");
        if (!is_focus_eligible(*client))
            return error_reply("window not focusable");

        focus_any_window(window);
        if (active_window_ != window)
            return error_reply("focus request refused");
        return ok_reply(std::to_string(window));
    }

    if (command.id == WindowList)
    {
        std::vector<std::pair<uint64_t, Client const*>> ordered;
        ordered.reserve(clients_.size());
        for (auto const& [id, client] : clients_)
        {
            if (client.kind() == Client::Kind::Dock || client.kind() == Client::Kind::Desktop)
                continue;
            ordered.push_back({ client.order, &client });
        }
        std::sort(ordered.begin(), ordered.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

        std::string json = "{\"focused\":" + std::to_string(active_window_) + ",\"windows\":[";
        bool first = true;
        for (auto const& [order, client] : ordered)
        {
            if (!first)
                json += ",";
            first = false;
            json += "{\"id\":" + std::to_string(client->id) + ",\"monitor\":" + std::to_string(client->monitor)
                + ",\"workspace\":" + std::to_string(client->workspace) + ",\"kind\":\""
                + client_kind_str(client->kind()) + "\"" + ",\"class\":\"" + json_escape(client->wm_class) + "\""
                + ",\"instance\":\"" + json_escape(client->wm_class_name) + "\"" + ",\"title\":\""
                + json_escape(client->name) + "\"" + ",\"focused\":" + (client->id == active_window_ ? "true" : "false")
                + ",\"fullscreen\":" + (client->fullscreen ? "true" : "false") + ",\"urgent\":"
                + (client->urgency.active() ? "true" : "false") + ",\"sticky\":" + (client->sticky ? "true" : "false")
                + ",\"iconic\":" + (client->iconic ? "true" : "false") + "}";
        }
        json += "]}";
        return ok_reply(json);
    }

    if (command.id == Stash)
    {
        if (active_window_ != XCB_NONE)
            stash_to_scratchpad(active_window_);
        return ok_reply("");
    }
    if (command.id == Cycle)
    {
        cycle_scratchpad_pool();
        return ok_reply("");
    }
    if (command.id == Toggle)
    {
        auto const& name = std::get<std::string>(command.argument);
        if (!find_named_scratchpad(name))
            return error_reply("unknown scratchpad: " + name);
        toggle_named_scratchpad(name);
        return ok_reply("");
    }
    if (command.id == CancelLaunch)
    {
        auto const& name = std::get<std::string>(command.argument);
        auto* state = find_named_scratchpad(name);
        if (!state)
            return error_reply("unknown scratchpad: " + name);
        if (state->pending_launch())
            state_.scratchpad_pending(name, false);
        return ok_reply("");
    }
    if (command.id == ScratchpadList)
    {
        std::string json = "{\"named\":[";
        for (size_t i = 0; i < named_scratchpads_.size(); ++i)
        {
            if (i > 0)
                json += ",";
            auto const& sp = named_scratchpads_[i];
            json += "{\"name\":\"" + json_escape(sp.name) + "\",\"window\":" + std::to_string(sp.window())
                + ",\"pending\":" + (sp.pending_launch() ? "true" : "false") + "}";
        }
        json += "],\"pool\":[";
        for (size_t i = 0; i < scratchpad_pool_.size(); ++i)
        {
            if (i > 0)
                json += ",";
            json += std::to_string(scratchpad_pool_[i]);
        }
        json += "]}";
        return ok_reply(json);
    }

    if (command.id == State)
    {
        auto query = [this](ipc::CommandId id) { return run_ipc_command(ipc::Command{ id, {} }).substr(3); };
        return ok_reply(
            "{\"instance\":\"" + ipc_.instance() + "\",\"sequence\":" + std::to_string(ipc_.sequence())
            + ",\"workspaces\":" + query(WorkspaceList) + ",\"windows\":" + query(WindowList)
            + ",\"scratchpads\":" + query(ScratchpadList) + "}"
        );
    }
    return error_reply("unknown command");
}

std::string WindowManager::handle_notification_attention(xcb_window_t target)
{
    auto* client = get_client(target);
    if (!client || (client->kind() != Client::Kind::Tiled && client->kind() != Client::Kind::Floating))
        return ok_reply("no-match");

    if (target == active_window_)
        return ok_reply("skipped-active");

    state_.urgency(client->id, UrgencySource::WmInitiated, true);
    return ok_reply(std::to_string(target));
}

} // namespace lwm
