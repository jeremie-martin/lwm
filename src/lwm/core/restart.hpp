#pragma once
#include "types.hpp"
#include <array>
#include <span>

namespace lwm::restart {
inline constexpr uint32_t state_version = 3;
inline constexpr uint32_t ratio_version = 3;
inline constexpr size_t legacy_client_words = 28;
inline constexpr size_t preference_words = 5;
inline constexpr size_t client_words = legacy_client_words + preference_words;

struct ClientRecord
{
    bool borderless = false;
    Geometry floating;
    std::optional<Geometry> prior_floating;
    uint32_t hidden_pool_kind = 0;
    std::optional<Client::Kind> kind;
    std::optional<uint8_t> urgency;
    std::optional<ClientPreferences> preferences;
    bool desktop_pinned = false;
};
struct GlobalRecord
{
    size_t focused_monitor;
    xcb_window_t active_window;
    bool showing_desktop;
    std::vector<std::pair<size_t, size_t>> workspaces;
};
struct LayoutRecord
{
    size_t monitor;
    size_t workspace;
    std::optional<LayoutStrategy> strategy;
    SplitRatioMap ratios;
};
std::array<uint32_t, client_words> encode_client(Client const& client);
std::optional<ClientRecord> decode_client(std::span<uint32_t const> words);
std::optional<GlobalRecord> decode_global(std::span<uint32_t const> words);
// Keep completed workspace records; a truncated workspace is never partially applied.
std::vector<LayoutRecord> decode_layouts(std::span<uint32_t const> words);
} // namespace lwm::restart
