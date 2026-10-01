#include "restart.hpp"
#include "policy.hpp"
#include <cstring>
#include <memory>
#include <rfl/AddTagsToVariants.hpp>
#include <rfl/NoExtraFields.hpp>
#include <rfl/NoOptionals.hpp>
#include <rfl/json/write.hpp>
#include <unordered_set>
#include <utility>

namespace lwm::restart {
namespace {

// Tighten the library reader at the handoff boundary: no ambiguous objects
// or variant tags, and no integer narrowing (including signed X11 coordinates).
struct Reader : rfl::json::Reader
{
    rfl::Result<InputObjectType> to_object(InputVarType value) const noexcept
    {
        auto object = rfl::json::Reader::to_object(value);
        if (!object)
            return object;
        std::unordered_set<std::string_view> names;
        size_t index, count;
        yyjson_val *key, *field;
        yyjson_obj_foreach(value.val_, index, count, key, field)
        {
            if (!names.emplace(yyjson_get_str(key), yyjson_get_len(key)).second)
                return rfl::error("duplicate restart field");
        }
        return object;
    }

    using rfl::json::Reader::read_object;
    template <typename Processors, typename... Fields>
    std::optional<rfl::Error> read_object(
        rfl::parsing::FieldVariantReader<Reader, rfl::json::Writer, Processors, Fields...> const& reader,
        InputObjectType object
    ) const noexcept
    {
        if (yyjson_obj_size(object.val_) != 1)
            return rfl::Error("restart mode must have exactly one tag");
        return rfl::json::Reader::read_object(reader, object);
    }

    template <typename T> rfl::Result<T> to_basic_type(InputVarType value) const noexcept
    {
        if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
        {
            if (yyjson_is_uint(value.val_) && std::in_range<T>(yyjson_get_uint(value.val_)))
                return static_cast<T>(yyjson_get_uint(value.val_));
            if (yyjson_is_sint(value.val_) && std::in_range<T>(yyjson_get_sint(value.val_)))
                return static_cast<T>(yyjson_get_sint(value.val_));
            return rfl::error("restart integer is out of range or has the wrong type");
        }
        else
            return rfl::json::Reader::to_basic_type<T>(value);
    }
};

using Wire = rfl::Processors<rfl::NoExtraFields, rfl::NoOptionals, rfl::AddTagsToVariants>;

// Structural validity belongs to the decoder. Relationships between records
// belong here, and must hold before any part of the snapshot is adopted.
bool valid(Snapshot const& snapshot)
{
    std::unordered_set<std::string> outputs;
    for (auto const& monitor : snapshot.monitors)
    {
        if (monitor.name.empty() || !outputs.insert(monitor.name).second || monitor.workspaces.empty()
            || monitor.current >= monitor.workspaces.size() || monitor.previous >= monitor.workspaces.size())
            return false;
        for (auto const& workspace : monitor.workspaces)
            for (auto const& [address, ratio] : workspace.ratios)
                if (!(ratio > 0 && ratio < 1))
                    return false;
    }
    if (!snapshot.monitors.empty() && snapshot.focused_monitor >= snapshot.monitors.size())
        return false;
    std::unordered_set<uint64_t> recencies;
    std::unordered_set<xcb_window_t> registered;
    for (auto window : snapshot.registration_order)
        if (window == XCB_NONE || !registered.insert(window).second)
            return false;
    for (auto const& client : snapshot.clients)
    {
        if (client.mru_order == UINT64_MAX || (client.mru_order && !recencies.insert(client.mru_order).second)
            || client.monitor >= snapshot.monitors.size()
            || client.workspace >= snapshot.monitors[client.monitor].workspaces.size()
            || client.urgency
                > (static_cast<uint8_t>(UrgencySource::WmInitiated) | static_cast<uint8_t>(UrgencySource::App))
            || !registered.erase(client.window))
            return false;
        if (auto const* floating = std::get_if<FloatingMode>(&client.mode);
            floating && floating->tile_slot && floating->tile_slot->output.empty())
            return false;
    }
    std::unordered_set<std::string> names;
    std::unordered_set<xcb_window_t> claimed;
    for (auto const& named : snapshot.named_scratchpads)
    {
        if (!names.insert(named.name).second)
            return false;
        if (named.window
            && (*named.window == XCB_NONE || !claimed.insert(*named.window).second || !snapshot.find(*named.window)))
            return false;
    }
    std::unordered_set<xcb_window_t> candidates;
    for (auto const& client : snapshot.clients) candidates.insert(client.window);
    for (auto window : snapshot.fullscreen_claims)
        if (!candidates.erase(window))
            return false;
    return true;
}

} // namespace

ClientRecord const* Snapshot::find(xcb_window_t window) const
{
    for (auto const& record : clients)
        if (record.window == window)
            return &record;
    return nullptr;
}

MonitorRecord capture_monitor(Monitor const& monitor)
{
    MonitorRecord record{ monitor.name, monitor.geometry(), monitor.current_workspace, monitor.previous_workspace, {} };
    for (auto const& workspace : monitor.workspaces)
        record.workspaces.push_back(
            { workspace.layout_strategy, workspace.split_ratios, workspace.windows, workspace.preferred_tile }
        );
    return record;
}

void Snapshot::rebind(std::span<Monitor const> discovered)
{
    assert(!discovered.empty());
    bool changed = monitors.size() != discovered.size();
    std::vector<Monitor> previous;
    for (size_t i = 0; i < monitors.size(); ++i)
    {
        auto const& saved = monitors[i];
        changed |=
            i >= discovered.size() || saved.name != discovered[i].name || saved.geometry != discovered[i].geometry();
        Monitor monitor;
        monitor.name = saved.name;
        monitor.current_workspace = saved.current;
        monitor.previous_workspace = saved.previous;
        for (auto const& workspace : saved.workspaces)
            monitor.workspaces.push_back(
                { workspace.tiles, workspace.preferred_tile, workspace.strategy, workspace.ratios }
            );
        previous.push_back(std::move(monitor));
    }
    std::vector<Monitor> targets(discovered.begin(), discovered.end());
    auto destinations = hotplug_policy::preserve_workspaces(previous, targets);
    focused_monitor = focused_monitor < destinations.size() ? destinations[focused_monitor] : 0;
    for (auto& client : clients)
    {
        size_t target = destinations.at(client.monitor);
        bool displaced = monitors[client.monitor].name != targets[target].name;
        client.monitor = target;
        client.workspace = std::min(client.workspace, targets[target].workspaces.size() - 1);
        if (changed)
        {
            client.fullscreen_monitors.reset();
            if (auto* floating = std::get_if<FloatingMode>(&client.mode))
                floating->geometry =
                    hotplug_policy::fit_floating(floating->geometry, targets[target].working_area(), displaced);
        }
        // Admission discards tile-return slots whose original workspace vanished.
    }
    monitors.clear();
    for (auto const& target : targets) monitors.push_back(capture_monitor(target));
}

std::vector<uint32_t> encode(Snapshot const& snapshot)
{
    // X11 output names are byte strings. Preserve them even if they are not UTF-8.
    auto payload = rfl::json::write<Wire>(snapshot, YYJSON_WRITE_ALLOW_INVALID_UNICODE);
    if (payload.size() > UINT32_MAX)
        throw std::length_error("Restart snapshot is too large");
    std::vector<uint32_t> words(2 + (payload.size() + 3) / 4);
    words[0] = format;
    words[1] = static_cast<uint32_t>(payload.size());
    std::memcpy(words.data() + 2, payload.data(), payload.size());
    return words;
}

std::optional<Snapshot> decode(std::span<uint32_t const> words)
{
    if (words.size() < 2 || words[0] != format || words.size() != 2 + (size_t{ words[1] } + 3) / 4)
        return std::nullopt;
    auto bytes = std::string_view(reinterpret_cast<char const*>(words.data() + 2), (words.size() - 2) * 4);
    if (bytes.substr(words[1]).find_first_not_of('\0') != bytes.npos)
        return std::nullopt;
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(
        yyjson_read(bytes.data(), words[1], YYJSON_READ_ALLOW_INVALID_UNICODE),
        &yyjson_doc_free
    );
    if (!document)
        return std::nullopt;
    auto snapshot = rfl::parsing::Parser<Reader, rfl::json::Writer, Snapshot, Wire>::read(
        Reader{},
        Reader::InputVarType(yyjson_doc_get_root(document.get()))
    );
    if (!snapshot || !valid(*snapshot))
        return std::nullopt;
    return std::move(*snapshot);
}

} // namespace lwm::restart
