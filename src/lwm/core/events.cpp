#include "events.hpp"
#include <array>
#include <meta>
#include <rfl/json/write.hpp>

namespace lwm {

std::span<std::string_view const> event_names()
{
    static constexpr auto names = []<size_t... I>(std::index_sequence<I...>)
    { return std::array{ std::meta::identifier_of(std::meta::dealias(^^std::variant_alternative_t<I, Event>))... }; }
    (std::make_index_sequence<std::variant_size_v<Event>>{});
    return names;
}

uint32_t parse_event_filter(std::string_view filter)
{
    if (filter.empty())
        return all_events;
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
        auto names = event_names();
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == token)
                mask |= uint32_t{ 1 } << i;
        pos = comma + 1;
    }
    return mask;
}

// X metadata can contain opaque bytes; preserve them as the existing IPC does.
std::string json_string(std::string_view input) { return rfl::json::write(input, YYJSON_WRITE_ALLOW_INVALID_UNICODE); }

namespace {
template <typename T> struct Envelope
{
    std::string_view instance;
    uint64_t sequence;
    std::string_view event;
    rfl::Flatten<T const*> payload;
};
}

std::string event_json(Event const& event, std::string_view instance, uint64_t sequence)
{
    return std::visit([&]<typename T>(T const& payload)
    {
        return rfl::json::write(Envelope<T>{ instance, sequence, std::meta::identifier_of(std::meta::dealias(^^T)), &payload },
                                YYJSON_WRITE_ALLOW_INVALID_UNICODE);
    }, event);
}

} // namespace lwm
