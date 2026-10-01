#pragma once

#include "restart.hpp"
#include "types.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lwm {

// The single owner of LWM's domain model. Readers receive const views; every
// mutation is a named operation that keeps membership, focus memory and
// scratchpad claims consistent. Visibility, fullscreen ownership and effective
// classification are derived on demand rather than stored.
class State
{
public:
    using Clients = std::unordered_map<xcb_window_t, Client>;
    using Fixtures = std::unordered_map<xcb_window_t, Fixture>;

    // Derived for one read pass, never retained between operations.
    // Ancestry is independent of iconic/workspace eligibility.
    struct FullscreenVisibility
    {
        std::vector<xcb_window_t> owners;
        std::unordered_set<xcb_window_t> exempt;

        bool suppressed(Client const& client) const
        {
            return owners[client.monitor] != XCB_NONE && !exempt.contains(client.id);
        }
    };

    struct NamedScratchpad
    {
        struct Empty
        { };
        struct LaunchPending
        { };
        struct Claimed
        {
            xcb_window_t window = XCB_NONE;
        };

        std::string name;
        std::variant<Empty, LaunchPending, Claimed> state = Empty{ };

        xcb_window_t window() const
        {
            auto const* claimed = std::get_if<Claimed>(&state);
            return claimed ? claimed->window : XCB_NONE;
        }
        bool pending_launch() const { return std::holds_alternative<LaunchPending>(state); }
    };

    enum class RelocationGeometry
    {
        Preserve,  ///< The caller already chose the floating rectangle
        Center,    ///< Center on the target workarea when the monitor changes
        Translate, ///< Keep the offset within the workarea when the monitor changes
    };

    // Registry
    Clients const& clients() const { return clients_; }
    Fixtures const& fixtures() const { return fixtures_; }
    std::vector<Monitor> const& monitors() const { return monitors_; }
    Client const* find(xcb_window_t id) const;
    std::vector<Client const*> clients_by_order() const;
    Client const& require(xcb_window_t id) const;
    Fixture const* find_fixture(xcb_window_t id) const;
    // The candidate's placement must be valid; tiled clients join their workspace.
    // During adoption, both registries use the same saved registration order.
    // Survivors receive their saved rank; newcomers follow all saved ranks.
    void insert(Client client, std::span<xcb_window_t const> registration_order = { });
    void insert_fixture(xcb_window_t id, Fixture::Role role, std::span<xcb_window_t const> registration_order = { });
    void erase(xcb_window_t id);

    // Derived views
    // A placement is shown when it is its monitor's current workspace and the desktop is not shown.
    bool shows(size_t monitor, size_t workspace) const;
    bool in_view(Client const& client) const;
    xcb_window_t fullscreen_owner(size_t monitor) const;
    std::vector<xcb_window_t> fullscreen_owners() const;
    FullscreenVisibility fullscreen_visibility() const;
    bool visible(Client const& client) const;
    // Hot loops share fullscreen owners and descendant membership.
    bool visible(Client const& client, FullscreenVisibility const& fullscreen) const;
    // Another window owns fullscreen here and this client is not its descendant.
    bool suppressed(Client const& client) const;
    static bool accepts_focus(Client const& client) { return client.accepts_input || client.supports_take_focus; }
    bool focusable(Client const& client) const;
    bool focusable(Client const& client, FullscreenVisibility const& fullscreen) const;
    uint64_t revision() const { return revision_; }
    // Counters let focus cycling detect registrations and recency changes.
    uint64_t next_order() const { return next_order_; }
    uint64_t next_recency() const { return next_recency_; }

    // Focus
    xcb_window_t active_window() const { return active_window_; }
    size_t focused_monitor() const { return focused_monitor_; }
    bool showing_desktop() const { return showing_desktop_; }
    void focus(xcb_window_t id, uint32_t time = 0);
    void focus_monitor(size_t monitor);
    void remember_focus(xcb_window_t id);
    void show_desktop(bool enabled);
    void request_focus_repair() { repair_focus_ = true; }
    // Completion consumes explicit focus requests (including same-window focus) and repair intent.
    std::optional<uint32_t> take_focus_request() { return std::exchange(focus_request_, std::nullopt); }
    bool take_focus_repair() { return std::exchange(repair_focus_, false); }

    // Placement and mode
    bool relocate(
        xcb_window_t id,
        size_t monitor,
        size_t workspace,
        RelocationGeometry geometry = RelocationGeometry::Preserve,
        std::optional<size_t> tile_index = std::nullopt
    );
    void floating(xcb_window_t id, bool enabled);
    void geometry(xcb_window_t id, Geometry rectangle);
    void place_tile(xcb_window_t id, Geometry rectangle);
    void swap_tiles(size_t monitor, size_t a, size_t b);

    // Client state
    void iconic(xcb_window_t id, bool enabled);
    void sticky(xcb_window_t id, bool enabled);
    // Assignment is idempotent. Entering fullscreen establishes a claim.
    void fullscreen(xcb_window_t id, bool enabled);
    // An interaction enables fullscreen and renews priority, even if already enabled.
    void request_fullscreen(xcb_window_t id);
    void maximize(xcb_window_t id, bool horizontal, bool vertical);
    void modal(xcb_window_t id, bool enabled);
    void borderless(xcb_window_t id, bool enabled);
    void layer(xcb_window_t id, LayerHint hint);
    void skip_taskbar(xcb_window_t id, bool enabled);
    void skip_pager(xcb_window_t id, bool enabled);
    void urgency(xcb_window_t id, UrgencySource source, bool enabled);
    void clear_urgency(xcb_window_t id);
    void fullscreen_monitors(xcb_window_t id, std::optional<FullscreenMonitors> value);
    void pin_desktop(xcb_window_t id, bool pinned);

    // Metadata
    void title(xcb_window_t id, std::string value);
    void window_class(xcb_window_t id, std::string instance, std::string name);
    void window_type(xcb_window_t id, WindowType type);
    void transient(xcb_window_t id, xcb_window_t parent);
    void focus_hints(xcb_window_t id, bool input, bool take_focus);
    void user_time(xcb_window_t id, uint32_t time, xcb_window_t window);
    void rule(xcb_window_t id, std::optional<RuleActions> actions);

    // Workspaces and monitors
    bool switch_workspace(size_t monitor, size_t workspace);
    void layout(size_t monitor, LayoutStrategy strategy);
    void ratio(size_t monitor, SplitAddress address, double value);
    void erase_ratio(size_t monitor, SplitAddress address);
    void reset_ratios(size_t monitor);
    void workarea(size_t monitor, Strut strut);
    // Rebind workspaces by output name, reassign clients, and fit floating
    // rectangles into the discovered workareas.
    void replace_monitors(std::vector<Monitor> monitors);

    // Scratchpads: the named slots and the pool are the only membership records.
    std::vector<NamedScratchpad> const& named_scratchpads() const { return named_scratchpads_; }
    std::vector<xcb_window_t> const& scratchpad_pool() const { return scratchpad_pool_; }
    NamedScratchpad const* named_scratchpad(std::string_view name) const;
    NamedScratchpad const* scratchpad_claim(xcb_window_t id) const;
    bool pooled(xcb_window_t id) const;
    void configure_scratchpads(std::span<std::string const> names);
    void claim_scratchpad(std::string_view name, xcb_window_t id);
    void pool_scratchpad(xcb_window_t id);
    void advance_scratchpad_pool();
    void scratchpad_pending(std::string_view name, bool pending);

    // Exec handoff
    restart::Snapshot snapshot() const;
    // Before adoption: resolve the handoff onto current outputs and restore the
    // workspace graph. The rebound records then place clients and rank membership.
    void restore_workspaces(restart::Snapshot& snapshot);
    // After adoption placed each saved client: order, focus memory, recency and claims.
    void restore_membership(restart::Snapshot const& snapshot);

    // Publication reads a frozen model; Debug builds reject mutation meanwhile.
    void freeze() { frozen_ = true; }
    void thaw() { frozen_ = false; }

private:
    Clients clients_;
    Fixtures fixtures_;
    std::vector<Monitor> monitors_;
    std::vector<NamedScratchpad> named_scratchpads_;
    std::vector<xcb_window_t> scratchpad_pool_;
    xcb_window_t active_window_ = XCB_NONE;
    size_t focused_monitor_ = 0;
    bool showing_desktop_ = false;
    std::optional<uint32_t> focus_request_;
    bool repair_focus_ = false;
    uint64_t next_order_ = 0;
    uint64_t next_recency_ = 0;
    uint64_t next_fullscreen_claim_ = 0;
    uint64_t revision_ = 0;
    bool frozen_ = false;

    uint64_t register_window(xcb_window_t id, std::span<xcb_window_t const> registration_order);
    Client& edit(xcb_window_t id);
    void touch(xcb_window_t id);
    std::vector<xcb_window_t> fullscreen_claim_order() const;
    Workspace& edit_workspace(size_t monitor, size_t workspace);
    void mutated();
    void set_mode(xcb_window_t id, bool floating);
    void apply_default_mode(xcb_window_t id);
    // Records a field change; an unchanged value is not a mutation.
    template <typename T, typename V> bool assign(xcb_window_t id, T Client::* field, V&& value)
    {
        auto& client = clients_.at(id);
        if (client.*field == value)
            return false;
        mutated();
        client.*field = std::forward<V>(value);
        return true;
    }
    void attach(Client const& client, std::optional<size_t> index = std::nullopt);
    std::optional<TileSlot> detach(Client const& client);
    void release_scratchpad(xcb_window_t id);
    void forget_missing_tile_slot(Client& client) const;
};

} // namespace lwm
