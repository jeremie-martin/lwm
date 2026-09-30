#include "restart.hpp"
#include <algorithm>
#include <bit>

namespace lwm::restart {
namespace {
constexpr uint32_t skip_taskbar = 1, skip_pager = 2, above = 4, below = 8;
void pack_geometry(uint32_t* out, Geometry g)
{
    out[0] = static_cast<uint16_t>(g.x);
    out[1] = static_cast<uint16_t>(g.y);
    out[2] = g.width;
    out[3] = g.height;
}
Geometry geometry(uint32_t const* data)
{
    return { static_cast<int16_t>(data[0]),
             static_cast<int16_t>(data[1]),
             static_cast<uint16_t>(data[2]),
             static_cast<uint16_t>(data[3]) };
}
void pack_optional(uint32_t* out, std::optional<Geometry> const& g)
{
    out[0] = g.has_value();
    if (g)
        pack_geometry(out + 1, *g);
}
std::optional<Geometry> optional_geometry(uint32_t const* data)
{
    return data[0] ? std::optional{ geometry(data + 1) } : std::nullopt;
}
// Only this cursor advances through variable-length records. Counts are checked
// against remaining words before allocation or iteration.
class Reader
{
public:
    explicit Reader(std::span<uint32_t const> words)
        : words_(words)
    { }
    std::span<uint32_t const> take(size_t count)
    {
        if (count > words_.size())
            return {};
        auto result = words_.first(count);
        words_ = words_.subspan(count);
        return result;
    }

private:
    std::span<uint32_t const> words_;
};
}
std::array<uint32_t, client_words> encode_client(Client const& client)
{
    std::array<uint32_t, client_words> data{};
    data[1] = client.borderless;
    std::optional<Geometry> prior;
    if (client.kind() == Client::Kind::Tiled)
        prior = prior_floating_geometry(client);
    else if (auto* hidden = hidden_tiled_pool_scratchpad(client))
        prior = hidden->prior_floating;
    pack_geometry(
        data.data() + 2,
        client.kind() == Client::Kind::Floating ? floating_geometry(client) : prior.value_or(Geometry{})
    );
    // Keep the old wire slots for cross-binary restart; live state has one normal rectangle.
    auto normal = client.kind() == Client::Kind::Floating ? std::optional{ floating_geometry(client) } : std::nullopt;
    pack_optional(data.data() + 6, client.fullscreen ? normal : std::nullopt);
    pack_optional(data.data() + 11, client.maximized_horz || client.maximized_vert ? normal : std::nullopt);
    pack_optional(data.data() + 16, prior);
    data[22] = is_hidden_tiled_pool_scratchpad(client) ? 1 : hidden_floating_pool_scratchpad(client) ? 2 : 0;
    data[23] = client.kind() == Client::Kind::Tiled ? 1 : 2;
    data[24] = client.urgency.sources;
    // Preserve the legacy prefix for an older binary adopting these clients.
    auto const& prefs = client.preferences;
    data[25] = (prefs.skip_taskbar.value_or(false) ? skip_taskbar : 0)
        | (prefs.skip_pager.value_or(false) ? skip_pager : 0) | (prefs.layer == LayerHint::Above ? above : 0)
        | (prefs.layer == LayerHint::Below ? below : 0);
    data[26] = client.fullscreen && prefs.layer ? static_cast<uint32_t>(*prefs.layer) + 1 : 0;
    data[27] = client.desktop_pinned;
    data[28] = 1; // Preference suffix version; 0=unset, 1=false, 2=true.
    data[29] = prefs.floating ? (*prefs.floating ? 2 : 1) : 0;
    data[30] = prefs.skip_taskbar ? (*prefs.skip_taskbar ? 2 : 1) : 0;
    data[31] = prefs.skip_pager ? (*prefs.skip_pager ? 2 : 1) : 0;
    data[32] = prefs.layer ? static_cast<uint32_t>(*prefs.layer) + 1 : 0;
    return data;
}
std::optional<ClientRecord> decode_client(std::span<uint32_t const> data)
{
    if (data.size() < 24 || data[22] > 2 || data[23] > 2)
        return std::nullopt;
    for (size_t offset : { 2u, 7u, 12u, 17u })
    {
        if (offset != 2 && data[offset - 1] == 0)
            continue;
        for (size_t i = offset; i < offset + 4; ++i)
            if (data[i] > 65535)
                return std::nullopt;
    }
    ClientRecord record;
    record.borderless = data[1] != 0;
    record.floating = optional_geometry(data.data() + 11)
                          .value_or(optional_geometry(data.data() + 6).value_or(geometry(data.data() + 2)));
    record.prior_floating = optional_geometry(data.data() + 16);
    record.hidden_pool_kind = data[22];
    if (data[23])
        record.kind = data[23] == 1 ? Client::Kind::Tiled : Client::Kind::Floating;
    if (data.size() >= 26)
    {
        record.urgency =
            data[24] & (static_cast<uint8_t>(UrgencySource::App) | static_cast<uint8_t>(UrgencySource::WmInitiated));
        ClientPreferences prefs;
        if (data[25] & skip_taskbar)
            prefs.skip_taskbar = true;
        if (data[25] & skip_pager)
            prefs.skip_pager = true;
        if (data[25] & above)
            prefs.layer = LayerHint::Above;
        else if (data[25] & below)
            prefs.layer = LayerHint::Below;
        if (data.size() >= 27 && data[26] > 0 && data[26] <= static_cast<uint32_t>(LayerHint::Below) + 1)
            prefs.layer = static_cast<LayerHint>(data[26] - 1);
        // Older records cannot distinguish an explicit choice from a default.
        // Retain the saved mode rather than replaying current rules on adoption.
        if (record.kind)
            prefs.floating = *record.kind == Client::Kind::Floating;
        record.preferences = prefs;
    }
    else if (data.size() == 25)
        record.urgency = data[24] ? static_cast<uint8_t>(UrgencySource::WmInitiated) : 0;
    record.desktop_pinned = data.size() >= 28 && data[27] != 0;
    if (data.size() >= 33 && data[28] == 1 && data[29] <= 2 && data[30] <= 2 && data[31] <= 2 && data[32] <= 3)
    {
        auto boolean = [](uint32_t value) -> std::optional<bool>
        { return value ? std::optional{ value == 2 } : std::nullopt; };
        record.preferences =
            ClientPreferences{ boolean(data[29]),
                               boolean(data[30]),
                               boolean(data[31]),
                               data[32] ? std::optional{ static_cast<LayerHint>(data[32] - 1) } : std::nullopt };
    }
    return record;
}
std::optional<GlobalRecord> decode_global(std::span<uint32_t const> data)
{
    if (data.size() < 5 || data[0] != state_version || data[4] > (data.size() - 5) / 2)
        return std::nullopt;
    GlobalRecord result{ data[1], data[2], data[3] != 0, {} };
    for (size_t i = 0; i < data[4]; ++i) result.workspaces.emplace_back(data[5 + i * 2], data[6 + i * 2]);
    return result;
}
std::vector<LayoutRecord> decode_layouts(std::span<uint32_t const> data)
{
    std::vector<LayoutRecord> result;
    Reader reader(data);
    auto header = reader.take(2);
    if (header.empty() || header[0] < 1 || header[0] > ratio_version)
        return result;
    uint32_t version = header[0];
    for (size_t m = 0; m < header[1]; ++m)
    {
        auto count = reader.take(1);
        if (count.empty())
            break;
        for (size_t w = 0; w < count[0]; ++w)
        {
            auto layout = reader.take(version >= 3 ? 2 : 1);
            if (layout.empty())
                return result;
            size_t stride = version == 1 ? 3 : 4;
            if (layout.back() > std::numeric_limits<size_t>::max() / stride)
                return result;
            auto entries = reader.take(static_cast<size_t>(layout.back()) * stride);
            if (layout.back() && entries.empty())
                return result;
            LayoutRecord record{ m, w, {}, {} };
            if (version >= 3 && layout[0] <= static_cast<uint32_t>(LayoutStrategy::Monocle))
                record.strategy = static_cast<LayoutStrategy>(layout[0]);
            for (size_t pos = 0; pos < entries.size(); pos += stride)
            {
                auto entry = entries.subspan(pos, stride);
                auto address = version == 1 ? deserialize_split_address(entry[0] & 0xFF, entry[0] >> 8)
                                            : deserialize_split_address(entry[0], entry[1]);
                double ratio = std::bit_cast<double>(
                    static_cast<uint64_t>(entry[stride - 2]) | (static_cast<uint64_t>(entry[stride - 1]) << 32)
                );
                if (address && ratio > 0 && ratio < 1)
                    record.ratios[*address] = ratio;
            }
            result.push_back(std::move(record));
        }
    }
    return result;
}
} // namespace lwm::restart
