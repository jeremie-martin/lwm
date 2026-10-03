#pragma once

#include "restart.hpp"
#include "types.hpp"
#include "lwm/layout/layout.hpp"
#include <expected>
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

// Owns domain state. Const views and named mutations preserve membership and
// claims; visibility, fullscreen ownership, and classification are derived.
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
    void insert(Client client);
    // Dock reservations shape every monitor's workarea while the dock is registered.
    void insert_fixture(xcb_window_t id, Fixture::Role role, DockStrut strut = { });
    void reserve(xcb_window_t id, DockStrut strut);
    void erase(xcb_window_t id);

    // The installed configuration. Layout, scratchpad slots and matching rules
    // follow it; the workspace count is fixed once monitors exist.
    Config const& config() const { return config_; }
    std::expected<void, std::string> configure(Config config);
    // Layout is derived, never written back into clients.
    Layout const& layout_engine() const { return layout_; }
    // One immutable per-pass view per managed client, in registration order.
    // A rectangle means visible; absence means hidden.
    struct Projected
    {
        Client const* client;
        std::optional<Geometry> geometry;
    };
    std::vector<xcb_window_t> tiled_participants(size_t monitor, FullscreenVisibility const& fullscreen) const;
    std::vector<Projected> project(FullscreenVisibility const& fullscreen) const;
    Geometry presentation_geometry(Client const& client) const;
    Geometry normal_geometry(Client const& client) const;

    // Derived views
    // A placement is shown when it is its monitor's current workspace and the desktop is not shown.
    bool shows(size_t monitor, size_t workspace) const;
    bool in_view(Client const& client) const;
    xcb_window_t fullscreen_owner(size_t monitor) const;
    std::vector<xcb_window_t> const& fullscreen_claims() const { return fullscreen_claims_; }
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
    // Monotonic bounds for registration and completed focus ranks.
    uint64_t next_order() const { return next_order_; }
    uint64_t next_recency() const { return next_recency_; }

    // Focus
    xcb_window_t active_window() const { return active_window_; }
    size_t focused_monitor() const { return focused_monitor_; }
    bool showing_desktop() const { return showing_desktop_; }
    // Activation restores a minimized client and selects its workspace. Unknown or
    // clients without input or WM_TAKE_FOCUS are refused; NONE clears focus.
    void focus(xcb_window_t id, uint32_t time = 0, bool record_user_time = true);
    void focus_fallback(size_t monitor, bool record_user_time = true);
    bool cycle_focus(bool forward);
    void restore(xcb_window_t id, bool activate);
    void focus_monitor(size_t monitor);
    void show_desktop(bool enabled);
    void request_focus_repair()
    {
        mutated();
        repair_focus_ = true;
    }
    // Resolve eligibility, recency and user time only for the final focus, once
    // per transition. input_time is the latest observed input timestamp.
    std::optional<uint32_t> complete_focus(uint32_t input_time = 0);

    // Placement and mode
    // Moving the active client follows a shown destination or chooses replacement
    // focus on the source monitor. Hidden destinations remember tile preference.
    bool relocate(
        xcb_window_t id,
        size_t monitor,
        size_t workspace,
        RelocationGeometry geometry = RelocationGeometry::Preserve,
        std::optional<size_t> tile_index = std::nullopt
    );
    void floating(xcb_window_t id, bool enabled);
    void geometry(xcb_window_t id, Geometry rectangle);
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
    void layer(xcb_window_t id, LayerHint hint);
    void skip_taskbar(xcb_window_t id, bool enabled);
    void skip_pager(xcb_window_t id, bool enabled);
    void urgency(xcb_window_t id, UrgencySource source, bool enabled);
    void clear_urgency(xcb_window_t id);
    void fullscreen_monitors(xcb_window_t id, std::optional<FullscreenMonitors> value);
    void pin_desktop(xcb_window_t id, bool pinned);

    // Metadata updates include classification, placement, changed rules and pending claims.
    void title(xcb_window_t id, std::string value);
    void window_class(xcb_window_t id, std::string instance, std::string name);
    void window_type(xcb_window_t id, WindowType type);
    void transient(xcb_window_t id, xcb_window_t parent, std::optional<Geometry> parent_preview = std::nullopt);
    void focus_hints(xcb_window_t id, bool input, bool take_focus);
    void user_time(xcb_window_t id, uint32_t time, xcb_window_t window);
    void apply_initial_rule(xcb_window_t id);

    // Workspaces and monitors
    bool switch_workspace(size_t monitor, size_t workspace);
    void layout(size_t monitor, LayoutStrategy strategy);
    void ratio(size_t monitor, SplitAddress address, double value);
    void erase_ratio(size_t monitor, SplitAddress address);
    void reset_ratios(size_t monitor);
    // Rebind workspaces by output name and reassign clients. A changed topology
    // fits floating rectangles into the new workareas and clears monitor hints.
    void replace_topology(Topology topology);

    // Scratchpads: the named slots and the pool are the only membership records.
    std::vector<NamedScratchpad> const& named_scratchpads() const { return named_scratchpads_; }
    std::vector<xcb_window_t> const& scratchpad_pool() const { return scratchpad_pool_; }
    NamedScratchpad const* named_scratchpad(std::string_view name) const;
    NamedScratchpad const* scratchpad_claim(xcb_window_t id) const;
    bool pooled(xcb_window_t id) const;
    ScratchpadConfig const* match_scratchpad(Client const& client) const;
    void claim_scratchpad(xcb_window_t id, ScratchpadConfig const& config);
    bool claim_pending_scratchpad(xcb_window_t id);
    // Returns the configuration whose command the shell should launch, if any.
    std::expected<ScratchpadConfig const*, std::string> toggle_scratchpad(std::string_view name);
    void pool_scratchpad(xcb_window_t id);
    void advance_scratchpad_pool();
    void scratchpad_pending(std::string_view name, bool pending);

    // Exec handoff
    restart::Snapshot snapshot() const;
    // Install a validated saved graph over freshly observed application state,
    // rebind it to discovered outputs, then admit newcomers in observation order.
    void restore_graph(restart::Snapshot const& snapshot, std::vector<Client> observed);

    // Publication reads a frozen model; Debug builds reject mutation meanwhile.
    void freeze() { frozen_ = true; }
    void thaw() { frozen_ = false; }

private:
    Geometry fullscreen_geometry(Client const& client) const;
    Config config_;
    Layout layout_;
    Geometry screen_;
    std::vector<xcb_window_t> workspace_tiles(
        size_t monitor, size_t workspace, FullscreenVisibility const* fullscreen, xcb_window_t include = XCB_NONE
    ) const;
    Clients clients_;
    Fixtures fixtures_;
    std::vector<Monitor> monitors_;
    std::vector<NamedScratchpad> named_scratchpads_;
    std::vector<xcb_window_t> scratchpad_pool_;
    xcb_window_t active_window_ = XCB_NONE;
    size_t focused_monitor_ = 0;
    bool showing_desktop_ = false;
    struct FocusRequest
    {
        uint32_t time;
        bool record_user_time;
    };
    std::optional<FocusRequest> focus_request_;
    std::vector<xcb_window_t> focus_cycle_;
    bool repair_focus_ = false;
    uint64_t next_order_ = 0;
    uint64_t next_recency_ = 1;
    std::vector<xcb_window_t> fullscreen_claims_;
    uint64_t revision_ = 0;
    bool frozen_ = false;

    uint64_t register_window(xcb_window_t id);
    Client& edit(xcb_window_t id);
    Workspace& edit_workspace(size_t monitor, size_t workspace);
    void mutated();
    void select_focus(xcb_window_t id, uint32_t time = 0, bool record_user_time = true);
    void set_mode(xcb_window_t id, bool floating);
    void apply_default_mode(xcb_window_t id);
    void apply_rule(xcb_window_t id, RuleActions const& rule);
    bool match_rule(xcb_window_t id);
    void reconcile_metadata(xcb_window_t id);
    // Records a field change; an unchanged value is not a mutation.
    template <typename Owner, typename T, typename V> bool assign(xcb_window_t id, T Owner::* field, V&& value)
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
    void show_named_scratchpad(xcb_window_t id, ScratchpadConfig const& config);
    void forget_missing_tile_slot(Client& client) const;
    void reconcile_scratchpads();
    void reapply_rules();
    Monitor fresh_monitor(std::string name, Geometry geometry) const;
    void rebind(std::vector<Monitor> monitors);
    void update_workareas();
};

} // namespace lwm
