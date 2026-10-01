#include "restart.hpp"
#include "policy.hpp"
#include <bit>
#include <cstring>
#include <unordered_set>

namespace lwm::restart {
namespace {

class Writer
{
public:
    void word(uint32_t value) { words_.push_back(value); }
    void flag(bool value) { word(value ? 1 : 0); }
    void count(size_t value) { word(static_cast<uint32_t>(value)); }
    void geometry(Geometry g)
    {
        word(static_cast<uint16_t>(g.x));
        word(static_cast<uint16_t>(g.y));
        word(g.width);
        word(g.height);
    }
    void optional_geometry(std::optional<Geometry> const& g)
    {
        flag(g.has_value());
        geometry(g.value_or(Geometry{ }));
    }
    // 0 = unset, 1 = false, 2 = true.
    void optional_bool(std::optional<bool> value) { word(value ? (*value ? 2 : 1) : 0); }
    void optional_layer(std::optional<LayerHint> value) { word(value ? static_cast<uint32_t>(*value) + 1 : 0); }
    void ratio(double value)
    {
        auto bits = std::bit_cast<uint64_t>(value);
        word(static_cast<uint32_t>(bits));
        word(static_cast<uint32_t>(bits >> 32));
    }
    void text(std::string const& value)
    {
        count(value.size());
        for (size_t i = 0; i < value.size(); i += 4)
        {
            uint32_t packed = 0;
            std::memcpy(&packed, value.data() + i, std::min<size_t>(4, value.size() - i));
            word(packed);
        }
    }
    std::vector<uint32_t> take() { return std::move(words_); }

private:
    std::vector<uint32_t> words_;
};

// Every read is bounds-checked; a failed read poisons the reader so decoding
// stops without partially trusting later fields.
class Reader
{
public:
    explicit Reader(std::span<uint32_t const> words)
        : words_(words)
    { }

    bool ok() const { return ok_; }
    bool done() const { return ok_ && words_.empty(); }

    uint32_t word()
    {
        if (words_.empty())
        {
            ok_ = false;
            return 0;
        }
        uint32_t value = words_.front();
        words_ = words_.subspan(1);
        return value;
    }
    uint32_t bounded(uint32_t max)
    {
        uint32_t value = word();
        if (value > max)
            ok_ = false;
        return value;
    }
    bool flag() { return bounded(1) != 0; }
    // A count cannot exceed the remaining words, which bounds every allocation.
    size_t count(size_t words_per_item = 1)
    {
        uint32_t value = word();
        if (words_per_item && value > words_.size() / words_per_item)
            ok_ = false;
        return ok_ ? value : 0;
    }
    Geometry geometry()
    {
        uint32_t x = bounded(0xFFFF), y = bounded(0xFFFF), width = bounded(0xFFFF), height = bounded(0xFFFF);
        return { static_cast<int16_t>(static_cast<uint16_t>(x)),
                 static_cast<int16_t>(static_cast<uint16_t>(y)),
                 static_cast<uint16_t>(width),
                 static_cast<uint16_t>(height) };
    }
    std::optional<Geometry> optional_geometry()
    {
        bool present = flag();
        auto g = geometry();
        return present ? std::optional{ g } : std::nullopt;
    }
    std::optional<bool> optional_bool()
    {
        auto value = bounded(2);
        return value ? std::optional{ value == 2 } : std::nullopt;
    }
    std::optional<LayerHint> optional_layer()
    {
        auto value = bounded(static_cast<uint32_t>(LayerHint::Below) + 1);
        return value ? std::optional{ static_cast<LayerHint>(value - 1) } : std::nullopt;
    }
    double ratio()
    {
        uint64_t low = word();
        uint64_t high = word();
        return std::bit_cast<double>(low | (high << 32));
    }
    std::string text()
    {
        size_t size = word();
        if (size > words_.size() * 4)
        {
            ok_ = false;
            return { };
        }
        std::string value(size, '\0');
        for (size_t i = 0; i < size; i += 4)
        {
            uint32_t packed = word();
            std::memcpy(value.data() + i, &packed, std::min<size_t>(4, size - i));
        }
        return value;
    }

private:
    std::span<uint32_t const> words_;
    bool ok_ = true;
};

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
    MonitorRecord record{ monitor.name,
                          monitor.geometry(),
                          monitor.current_workspace,
                          monitor.previous_workspace,
                          { } };
    for (auto const& workspace : monitor.workspaces)
        record.workspaces.push_back(
            { workspace.layout_strategy, workspace.split_ratios, workspace.windows, workspace.focused_window }
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
                { workspace.tiles, workspace.focused, { }, workspace.strategy, workspace.ratios }
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
            if (client.kind == Client::Kind::Floating)
                client.geometry =
                    hotplug_policy::fit_floating(client.geometry, targets[target].working_area(), displaced);
        }
        // Admission discards tile-return slots whose original workspace vanished.
    }
    monitors.clear();
    for (auto const& target : targets) monitors.push_back(capture_monitor(target));
}

std::vector<uint32_t> encode(Snapshot const& snapshot)
{
    Writer out;
    out.word(format);
    out.count(snapshot.focused_monitor);
    out.word(snapshot.active);
    out.flag(snapshot.showing_desktop);
    out.count(snapshot.monitors.size());
    for (auto const& monitor : snapshot.monitors)
    {
        out.text(monitor.name);
        out.geometry(monitor.geometry);
        out.count(monitor.current);
        out.count(monitor.previous);
        out.count(monitor.workspaces.size());
        for (auto const& workspace : monitor.workspaces)
        {
            out.word(static_cast<uint32_t>(workspace.strategy));
            out.word(workspace.focused);
            out.count(workspace.ratios.size());
            for (auto const& [address, ratio] : workspace.ratios)
            {
                out.word(address.index);
                out.ratio(ratio);
            }
            out.count(workspace.tiles.size());
            for (auto window : workspace.tiles) out.word(window);
        }
    }
    out.count(snapshot.clients.size());
    for (auto const& client : snapshot.clients)
    {
        out.word(client.window);
        out.count(client.monitor);
        out.count(client.workspace);
        out.flag(client.kind == Client::Kind::Floating);
        out.geometry(client.geometry);
        out.optional_geometry(client.floating);
        out.optional_bool(client.preferences.floating);
        out.optional_bool(client.preferences.skip_taskbar);
        out.optional_bool(client.preferences.skip_pager);
        out.optional_layer(client.preferences.layer);
        out.word(client.urgency);
        out.flag(client.borderless);
        out.flag(client.desktop_pinned);
        out.flag(client.tile_slot.has_value());
        if (client.tile_slot)
        {
            out.count(client.tile_slot->index);
            out.text(client.tile_slot->output);
            out.count(client.tile_slot->workspace);
        }
        out.flag(client.fullscreen_monitors.has_value());
        if (auto const& m = client.fullscreen_monitors)
        {
            out.word(m->top);
            out.word(m->bottom);
            out.word(m->left);
            out.word(m->right);
        }
    }
    out.count(snapshot.named_scratchpads.size());
    for (auto const& named : snapshot.named_scratchpads)
    {
        out.text(named.name);
        out.flag(named.window.has_value());
        if (named.window)
            out.word(*named.window);
    }
    out.count(snapshot.pool.size());
    for (auto window : snapshot.pool) out.word(window);
    out.count(snapshot.fullscreen_claims.size());
    for (auto window : snapshot.fullscreen_claims) out.word(window);
    return out.take();
}

std::optional<Snapshot> decode(std::span<uint32_t const> words)
{
    Reader in(words);
    if (in.word() != format || !in.ok())
        return std::nullopt;
    Snapshot snapshot;
    snapshot.focused_monitor = in.word();
    snapshot.active = in.word();
    snapshot.showing_desktop = in.flag();
    snapshot.monitors.resize(in.count(8));
    std::unordered_set<std::string> outputs;
    for (auto& monitor : snapshot.monitors)
    {
        monitor.name = in.text();
        if (monitor.name.empty() || !outputs.insert(monitor.name).second)
            return std::nullopt;
        monitor.geometry = in.geometry();
        monitor.current = in.word();
        monitor.previous = in.word();
        monitor.workspaces.resize(in.count(4));
        if (monitor.workspaces.empty() || monitor.current >= monitor.workspaces.size()
            || monitor.previous >= monitor.workspaces.size())
            return std::nullopt;
        for (auto& workspace : monitor.workspaces)
        {
            workspace.strategy = static_cast<LayoutStrategy>(in.bounded(static_cast<uint32_t>(LayoutStrategy::Monocle)));
            workspace.focused = in.word();
            size_t ratios = in.count(3);
            for (size_t i = 0; i < ratios; ++i)
            {
                SplitAddress address{ in.word() };
                double ratio = in.ratio();
                if (!(ratio > 0 && ratio < 1))
                    return std::nullopt;
                workspace.ratios[address] = ratio;
            }
            workspace.tiles.resize(in.count());
            for (auto& window : workspace.tiles) window = in.word();
        }
    }
    if (!snapshot.monitors.empty() && snapshot.focused_monitor >= snapshot.monitors.size())
        return std::nullopt;
    snapshot.clients.resize(in.count(22));
    for (auto& client : snapshot.clients)
    {
        client.window = in.word();
        client.monitor = in.word();
        client.workspace = in.word();
        if (client.monitor >= snapshot.monitors.size()
            || client.workspace >= snapshot.monitors[client.monitor].workspaces.size())
            return std::nullopt;
        client.kind = in.flag() ? Client::Kind::Floating : Client::Kind::Tiled;
        client.geometry = in.geometry();
        client.floating = in.optional_geometry();
        client.preferences.floating = in.optional_bool();
        client.preferences.skip_taskbar = in.optional_bool();
        client.preferences.skip_pager = in.optional_bool();
        client.preferences.layer = in.optional_layer();
        client.urgency = static_cast<uint8_t>(
            in.bounded(static_cast<uint32_t>(UrgencySource::WmInitiated) | static_cast<uint32_t>(UrgencySource::App))
        );
        client.borderless = in.flag();
        client.desktop_pinned = in.flag();
        if (in.flag())
        {
            TileSlot slot;
            slot.index = in.word();
            slot.output = in.text();
            slot.workspace = in.word();
            if (client.kind != Client::Kind::Floating || slot.output.empty())
                return std::nullopt;
            client.tile_slot = std::move(slot);
        }
        if (in.flag())
            client.fullscreen_monitors = FullscreenMonitors{ in.word(), in.word(), in.word(), in.word() };
    }
    snapshot.named_scratchpads.resize(in.count(2));
    std::unordered_set<std::string> names;
    std::unordered_set<xcb_window_t> claimed;
    for (auto& named : snapshot.named_scratchpads)
    {
        named.name = in.text();
        if (!names.insert(named.name).second)
            return std::nullopt;
        if (in.flag())
        {
            auto window = in.word();
            if (window == XCB_NONE || !claimed.insert(window).second || !snapshot.find(window))
                return std::nullopt;
            named.window = window;
        }
    }
    snapshot.pool.resize(in.count());
    for (auto& window : snapshot.pool) window = in.word();
    snapshot.fullscreen_claims.resize(in.count());
    // Claims name distinct saved clients. Missing live clients are handled at
    // adoption, not by trusting dangling or duplicate IDs in the wire record.
    std::unordered_set<xcb_window_t> candidates;
    for (auto const& client : snapshot.clients)
        if (client.window != XCB_NONE)
            candidates.insert(client.window);
    for (auto& window : snapshot.fullscreen_claims)
    {
        window = in.word();
        if (!candidates.erase(window))
            return std::nullopt;
    }
    if (!in.done())
        return std::nullopt;
    return snapshot;
}

} // namespace lwm::restart
