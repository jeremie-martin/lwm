#include "events.hpp"
#include <cstdio>

namespace lwm {

uint32_t parse_event_filter(std::string_view filter)
{
    if (filter.empty())
        return Event_All;
    uint32_t mask = 0;
    size_t pos = 0;
    while (pos < filter.size())
    {
        size_t comma = filter.find(',', pos);
        if (comma == std::string_view::npos)
            comma = filter.size();
        size_t start = pos;
        while (start < comma && filter[start] == ' ')
            ++start;
        size_t end = comma;
        while (end > start && filter[end - 1] == ' ')
            --end;
        auto token = filter.substr(start, end - start);
        for (auto const& entry : event_specs)
            if (entry.name == token)
                mask |= entry.type;
        pos = comma + 1;
    }
    return mask;
}

std::string json_escape(std::string_view input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input)
    {
        switch (ch)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                }
                else
                    out += ch;
                break;
        }
    }
    return out;
}

namespace {
template <class... Ts> struct Overloaded : Ts...
{
    using Ts::operator()...;
};

std::string quoted(std::string_view text) { return "\"" + json_escape(text) + "\""; }

std::string placement_json(std::optional<Placement> const& placement)
{
    if (!placement)
        return { };
    return ",\"monitor\":" + std::to_string(placement->monitor) + ",\"workspace\":" + std::to_string(placement->workspace);
}
}

EventType event_type(Event const& event)
{
    return std::visit(
        Overloaded{
            [](event::WorkspaceSwitch const&) { return Event_WorkspaceSwitch; },
            [](event::FocusChange const&) { return Event_FocusChange; },
            [](event::WindowMap const&) { return Event_WindowMap; },
            [](event::WindowUnmap const&) { return Event_WindowUnmap; },
            [](event::LayoutChange const&) { return Event_LayoutChange; },
            [](event::KeyAction const&) { return Event_KeyAction; },
            [](event::ConfigReload const&) { return Event_ConfigReload; },
        },
        event
    );
}

int event_priority(Event const& event)
{
    switch (event.index())
    {
        case 0:
            return 0;
        case 1:
            return 1;
        case 2:
        case 3:
            return 2;
        default:
            return 3;
    }
}

std::string event_json(Event const& event)
{
    return std::visit(
        Overloaded{
            [](event::WorkspaceSwitch const& e)
            {
                return "{\"event\":\"workspace_switch\",\"monitor\":" + std::to_string(e.monitor)
                    + ",\"from\":" + std::to_string(e.from) + ",\"to\":" + std::to_string(e.to) + "}";
            },
            [](event::FocusChange const& e)
            {
                return "{\"event\":\"focus_change\",\"window\":" + std::to_string(e.window)
                    + ",\"class\":" + quoted(e.wm_class) + ",\"title\":" + quoted(e.title) + "}";
            },
            [](event::WindowMap const& e)
            {
                return "{\"event\":\"window_map\",\"window\":" + std::to_string(e.window)
                    + ",\"class\":" + quoted(e.wm_class) + ",\"kind\":" + quoted(e.kind)
                    + placement_json(e.placement) + "}";
            },
            [](event::WindowUnmap const& e)
            {
                return "{\"event\":\"window_unmap\",\"window\":" + std::to_string(e.window) + ",\"kind\":"
                    + quoted(e.kind) + placement_json(e.placement) + "}";
            },
            [](event::LayoutChange const& e)
            {
                std::string json = "{\"event\":\"layout_change\",\"action\":" + quoted(e.action);
                if (e.value)
                    json += ",\"value\":" + *e.value;
                if (e.delta)
                    json += ",\"delta\":" + std::to_string(*e.delta);
                return json + "}";
            },
            [](event::KeyAction const& e) { return "{\"event\":\"key_action\",\"action\":" + quoted(e.action) + "}"; },
            [](event::ConfigReload const& e)
            {
                std::string json = "{\"event\":\"config_reload\",\"success\":" + std::string(e.success ? "true" : "false")
                    + ",\"source\":" + quoted(e.source);
                if (!e.success)
                    json += ",\"error\":" + quoted(e.error);
                return json + "}";
            },
        },
        event
    );
}

} // namespace lwm
