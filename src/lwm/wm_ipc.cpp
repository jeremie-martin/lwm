#include "wm.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace lwm {
namespace {
std::string trim_ascii(std::string_view value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;

    return std::string(value.substr(start, end - start));
}

std::string ok_reply(std::string const& message)
{
    if (message.empty())
        return "ok";
    return "ok " + message;
}

std::string error_reply(std::string const& message) { return "error " + message; }

std::optional<double> parse_finite_number(std::string_view text)
{
    if (text.starts_with('+'))
        text.remove_prefix(1);
    double value;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

std::optional<xcb_window_t> parse_window_id(std::string_view value)
{
    uint32_t xid = 0;
    int base = 10;
    std::string_view digits = value;

    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
    {
        base = 16;
        digits.remove_prefix(2);
    }

    if (digits.empty())
        return std::nullopt;

    auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), xid, base);
    if (ec != std::errc{} || ptr != digits.data() + digits.size())
        return std::nullopt;

    return static_cast<xcb_window_t>(xid);
}

}

void WindowManager::emit_event(EventType type, std::string_view json)
{
    if (!ipc_.has_subscribers())
        return;
    // Observers must not receive an event before its pending X ordering updates.
    flush_stacking_list();
    conn_.flush();
    ipc_.emit(type, json);
}

std::string WindowManager::run_ipc_command(std::string const& command)
{
    auto const& trimmed = command;
    if (trimmed.empty())
        return error_reply("empty command");

    if (trimmed == "ping")
        return ok_reply("pong");

    if (trimmed == "version")
        return ok_reply(LWM_VERSION);

    if (trimmed == "reload-config")
    {
        auto result = reload_config();
        emit_config_reload_result(result, "ipc");
        if (!result)
            return error_reply(result.error());
        return ok_reply("reloaded");
    }

    if (trimmed == "restart")
    {
        initiate_restart();
        return ok_reply("restarting");
    }

    if (trimmed.starts_with("exec "))
    {
        std::string binary = trim_ascii(trimmed.substr(5));
        if (binary.empty())
            return error_reply("exec requires a binary path");
        initiate_restart(std::move(binary));
        return ok_reply("restarting");
    }

    // Layout commands
    {
        constexpr std::string_view layout_set_prefix = "layout set ";
        if (trimmed.starts_with(layout_set_prefix))
        {
            std::string name = trim_ascii(trimmed.substr(layout_set_prefix.size()));
            if (focused_monitor_ >= monitors_.size())
                return error_reply("no focused monitor");
            auto strategy = parse_layout_strategy(name);
            if (!strategy)
                return error_reply("unknown layout: " + name);
            focused_monitor().current().layout_strategy = *strategy;
            rearrange_monitor(focused_monitor(), true);
            flush_and_drain_crossing();
            std::string strategy_name = layout_strategy_str(*strategy);
            emit_event(
                Event_LayoutChange,
                "{\"event\":\"layout_change\",\"action\":\"layout_set\",\"value\":\"" + strategy_name + "\"}"
            );
            return ok_reply("layout set to " + strategy_name);
        }
    }

    {
        constexpr std::string_view ratio_set_prefix = "ratio set ";
        if (trimmed.starts_with(ratio_set_prefix))
        {
            std::string val_str = trim_ascii(trimmed.substr(ratio_set_prefix.size()));
            auto parsed = parse_finite_number(val_str);
            if (!parsed)
                return error_reply("invalid ratio value: " + val_str);
            double val = *parsed;
            double min_r = config_.layout.min_ratio;
            if (val < min_r || val > 1.0 - min_r)
                return error_reply(
                    "ratio out of range [" + std::to_string(min_r) + ", " + std::to_string(1.0 - min_r) + "]"
                );
            if (focused_monitor_ >= monitors_.size())
                return error_reply("no focused monitor");
            focused_monitor().current().split_ratios[SplitAddress{ 0 }] = val;
            rearrange_monitor(focused_monitor(), true);
            emit_event(
                Event_LayoutChange,
                "{\"event\":\"layout_change\",\"action\":\"ratio_set\",\"value\":" + std::to_string(val) + "}"
            );
            return ok_reply("ratio set");
        }
    }

    if (trimmed == "ratio reset")
    {
        if (focused_monitor_ >= monitors_.size())
            return error_reply("no focused monitor");
        focused_monitor().current().split_ratios.clear();
        rearrange_monitor(focused_monitor(), true);
        emit_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"ratio_reset\"}");
        return ok_reply("ratios reset");
    }

    {
        constexpr std::string_view ratio_adj_prefix = "ratio adjust ";
        if (trimmed.starts_with(ratio_adj_prefix))
        {
            std::string delta_str = trim_ascii(trimmed.substr(ratio_adj_prefix.size()));
            auto parsed = parse_finite_number(delta_str);
            if (!parsed)
                return error_reply("invalid delta value: " + delta_str);
            double delta = *parsed;
            if (focused_monitor_ >= monitors_.size())
                return error_reply("no focused monitor");
            if (!adjust_master_ratio(delta))
                return ok_reply("ratio unchanged");
            emit_event(
                Event_LayoutChange,
                "{\"event\":\"layout_change\",\"action\":\"ratio_adjust\",\"delta\":" + std::to_string(delta) + "}"
            );
            return ok_reply("ratio adjusted");
        }
    }

    {
        constexpr std::string_view notify_prefix = "notify-attention";
        constexpr std::string_view window_prefix = "window=";
        if (trimmed == notify_prefix || trimmed.starts_with("notify-attention "))
        {
            std::string arg = trim_ascii(trimmed.substr(notify_prefix.size()));
            // After trimming edges, any remaining whitespace means extra tokens.
            if (!arg.starts_with(window_prefix) || arg.find_first_of(" \t\r\n") != std::string::npos)
                return error_reply("usage: notify-attention window=<xid>");

            std::string_view value = std::string_view(arg).substr(window_prefix.size());
            auto window = parse_window_id(value);
            if (!window)
                return error_reply("invalid window id: " + std::string(value));

            return handle_notification_attention(*window);
        }
    }

    {
        constexpr std::string_view ws_switch_prefix = "workspace switch ";
        if (trimmed.starts_with(ws_switch_prefix))
        {
            std::string arg = trim_ascii(trimmed.substr(ws_switch_prefix.size()));
            if (arg.empty() || arg.find_first_of(" \t\r\n") != std::string::npos)
                return error_reply("usage: workspace switch <index>");

            size_t target = 0;
            auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), target);
            if (ec != std::errc{} || ptr != arg.data() + arg.size())
                return error_reply("invalid workspace index: " + arg);

            if (focused_monitor_ >= monitors_.size())
                return error_reply("no focused monitor");
            if (target >= focused_monitor().workspaces.size())
                return error_reply("workspace out of range");

            switch_workspace(target);
            return ok_reply(std::to_string(target));
        }
    }

    if (trimmed == "workspace next" || trimmed == "workspace prev")
    {
        if (focused_monitor_ >= monitors_.size())
            return error_reply("no focused monitor");
        size_t count = focused_monitor().workspaces.size();
        if (count == 0)
            return error_reply("no workspaces");
        size_t current = focused_monitor().current_workspace;
        size_t target = (trimmed == "workspace next") ? (current + 1) % count : (current + count - 1) % count;
        switch_workspace(target);
        return ok_reply(std::to_string(target));
    }

    if (trimmed == "workspace list")
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

    if (trimmed == "focus next" || trimmed == "focus prev")
    {
        if (focused_monitor_ >= monitors_.size())
            return error_reply("no focused monitor");
        if (!cycle_focus(trimmed == "focus next"))
            return error_reply("no focus candidates");
        return ok_reply(std::to_string(active_window_));
    }

    {
        constexpr std::string_view window_prefix = "window=";
        if (trimmed.starts_with("focus "))
        {
            std::string arg = trim_ascii(trimmed.substr(std::string_view("focus").size()));
            if (!arg.starts_with(window_prefix) || arg.find_first_of(" \t\r\n") != std::string::npos)
                return error_reply("usage: focus <next|prev|window=<xid>>");

            std::string_view value = std::string_view(arg).substr(window_prefix.size());
            auto window = parse_window_id(value);
            if (!window)
                return error_reply("invalid window id: " + std::string(value));

            auto* client = get_client(*window);
            if (!client)
                return error_reply("unknown window");
            if (!is_focus_eligible(*client))
                return error_reply("window not focusable");

            focus_any_window(*window);
            if (active_window_ != *window)
                return error_reply("focus request refused");
            return ok_reply(std::to_string(*window));
        }
    }

    if (trimmed == "window list")
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

    if (trimmed == "scratchpad stash")
    {
        if (active_window_ != XCB_NONE)
            stash_to_scratchpad(active_window_);
        return ok_reply("");
    }
    if (trimmed == "scratchpad cycle")
    {
        cycle_scratchpad_pool();
        return ok_reply("");
    }
    if (trimmed.starts_with("scratchpad toggle "))
    {
        std::string name(trimmed.substr(18));
        toggle_named_scratchpad(name);
        return ok_reply("");
    }
    if (trimmed == "scratchpad list")
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

    return error_reply("unknown command");
}

std::string WindowManager::handle_notification_attention(xcb_window_t target)
{
    auto* client = get_client(target);
    if (!client || (client->kind() != Client::Kind::Tiled && client->kind() != Client::Kind::Floating))
        return ok_reply("no-match");

    if (target == active_window_)
        return ok_reply("skipped-active");

    set_client_urgency(*client, UrgencySource::WmInitiated, true);
    return ok_reply(std::to_string(target));
}

} // namespace lwm
