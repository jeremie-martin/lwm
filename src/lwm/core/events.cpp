#include "events.hpp"
#include "lwm/core/overloaded.hpp"
#include <rfl/json/write.hpp>
#include <rfl/make_named_tuple.hpp>

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

// X metadata can contain opaque bytes; preserve them as the existing IPC does.
std::string json_string(std::string_view input) { return rfl::json::write(input, YYJSON_WRITE_ALLOW_INVALID_UNICODE); }

namespace {
template <typename... Fields> std::string json(std::string_view event, Fields&&... fields)
{
    return rfl::json::write(
        rfl::make_named_tuple(rfl::make_field<"event">(event), std::forward<Fields>(fields)...),
        YYJSON_WRITE_ALLOW_INVALID_UNICODE
    );
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
    using rfl::make_field;
    return std::visit(
        Overloaded{
            [](event::WorkspaceSwitch const& e)
            {
                return json(
                    "workspace_switch",
                    make_field<"monitor">(e.monitor),
                    make_field<"from">(e.from),
                    make_field<"to">(e.to)
                );
            },
            [](event::FocusChange const& e)
            {
                return json(
                    "focus_change",
                    make_field<"window">(e.window),
                    make_field<"class">(e.wm_class),
                    make_field<"title">(e.title)
                );
            },
            [](event::WindowMap const& e)
            {
                return json(
                    "window_map",
                    make_field<"window">(e.window),
                    make_field<"class">(e.wm_class),
                    make_field<"kind">(e.kind),
                    make_field<"monitor">(e.placement.transform([](auto p) { return p.monitor; })),
                    make_field<"workspace">(e.placement.transform([](auto p) { return p.workspace; }))
                );
            },
            [](event::WindowUnmap const& e)
            {
                return json(
                    "window_unmap",
                    make_field<"window">(e.window),
                    make_field<"kind">(e.kind),
                    make_field<"monitor">(e.placement.transform([](auto p) { return p.monitor; })),
                    make_field<"workspace">(e.placement.transform([](auto p) { return p.workspace; }))
                );
            },
            [](event::LayoutChange const& e)
            {
                return json(
                    "layout_change",
                    make_field<"action">(e.action),
                    make_field<"value">(e.value),
                    make_field<"delta">(e.delta)
                );
            },
            [](event::KeyAction const& e) { return json("key_action", make_field<"action">(e.action)); },
            [](event::ConfigReload const& e)
            {
                return json(
                    "config_reload",
                    make_field<"success">(e.success),
                    make_field<"source">(e.source),
                    make_field<"error">(e.success ? std::nullopt : std::optional{ e.error })
                );
            },
        },
        event
    );
}

} // namespace lwm
