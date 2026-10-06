#include "events.hpp"
#include <algorithm>
#include <array>
#include <ranges>
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
    auto names = event_names();
    for (auto part : std::views::split(filter, ','))
    {
        std::string_view token(part.begin(), part.end());
        token.remove_prefix(std::min(token.find_first_not_of(' '), token.size()));
        token.remove_suffix(token.size() - std::min(token.find_last_not_of(' ') + 1, token.size()));
        if (auto it = std::ranges::find(names, token); it != names.end())
            mask |= uint32_t{ 1 } << (it - names.begin());
    }
    return mask;
}

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
