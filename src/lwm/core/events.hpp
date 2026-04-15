#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lwm {

enum EventType : uint32_t
{
    Event_WindowMap = 1 << 0,
    Event_WindowUnmap = 1 << 1,
    Event_FocusChange = 1 << 2,
    Event_WorkspaceSwitch = 1 << 3,
    Event_LayoutChange = 1 << 4,
    Event_ConfigReload = 1 << 5,
    Event_KeyAction = 1 << 6,
    Event_All = 0xFFFFFFFF,
};

/// Parse a comma-separated filter string (e.g. "focus_change,window_map") into bitmask.
/// Returns Event_All if filter is empty.
inline uint32_t parse_event_filter(std::string_view filter)
{
    if (filter.empty())
        return Event_All;

    struct NamedEvent
    {
        std::string_view name;
        EventType type;
    };
    static constexpr NamedEvent table[] = {
        {"window_map", Event_WindowMap},
        {"window_unmap", Event_WindowUnmap},
        {"focus_change", Event_FocusChange},
        {"workspace_switch", Event_WorkspaceSwitch},
        {"layout_change", Event_LayoutChange},
        {"config_reload", Event_ConfigReload},
        {"key_action", Event_KeyAction},
    };

    uint32_t mask = 0;
    size_t pos = 0;
    while (pos < filter.size())
    {
        size_t comma = filter.find(',', pos);
        if (comma == std::string_view::npos)
            comma = filter.size();

        // Trim whitespace around token
        size_t start = pos;
        while (start < comma && filter[start] == ' ')
            ++start;
        size_t end = comma;
        while (end > start && filter[end - 1] == ' ')
            --end;

        std::string_view token = filter.substr(start, end - start);
        for (auto const& entry : table)
        {
            if (entry.name == token)
            {
                mask |= entry.type;
                break;
            }
        }
        pos = comma + 1;
    }

    return mask;
}

/// Escape a string for JSON output (handles quotes, backslashes, control chars).
inline std::string json_escape(std::string_view input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input)
    {
        switch (ch)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                }
                else
                {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

} // namespace lwm
