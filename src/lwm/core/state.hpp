#pragma once

#include "floating.hpp"
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

    // A pointer interaction. The shell holds the pointer grab while one exists.
    struct WindowDrag
    {
        xcb_window_t window;
        Client::Kind kind;
        size_t monitor;
        size_t workspace;
        Geometry start_geometry;
        floating::ResizeEdge edges;
    };
    struct SplitDrag
    {
        size_t monitor;
        size_t workspace;
        SplitHitResult split;
        Geometry area;
        LayoutStrategy strategy;
        std::vector<xcb_window_t> participants;
    };
    struct Drag
    {
        std::variant<WindowDrag, SplitDrag> operation;
        int16_t start_x, start_y;
        int16_t last_x, last_y;
        uint8_t button; ///< Zero accepts any release (unspecified EWMH button)
    };
    struct SplitHit
    {
        SplitHitResult hit;
        size_t monitor;
    };
    // An interaction to begin once the shell holds the pointer.
    struct Grip
    {
        xcb_window_t window;
        floating::ResizeEdge edges;
    };
    using Interaction = std::variant<Grip, SplitHit>;
    struct Press
    {
        bool consumed; ///< The client does not receive the click
        std::optional<Interaction> interaction;
    };

    enum class RelocationGeometry
    {
        Preserve,  ///< The caller already chose the floating rectangle
        Center,    ///< Center on the target workarea when the monitor changes
        Translate, ///< Keep the offset within the workarea when the monitor changes
    };

    // Registry and admission
    Clients const& clients() const { return clients_; }
    Fixtures const& fixtures() const { return fixtures_; }
    std::vector<Monitor> const& monitors() const { return monitors_; }
    Client const* find(xcb_window_t id) const;
    Client const& require(xcb_window_t id) const;
    Fixture const* find_fixture(xcb_window_t id) const;
    std::vector<Client const*> clients_by_order() const;
    // Established identity wins: saved fixtures keep their role and saved
    // clients stay clients. Only newcomers choose a role from current metadata.
    static WindowRole role(xcb_window_t id, WindowType type, bool transient, restart::Snapshot const* handoff);
    // A live map: classify, register, place and focus one window. Docks and
    // desktops become fixtures; popups stay unregistered for the shell to map.
    void admit(WindowObservation const& window);
    // Startup: register the whole scene, restore a valid predecessor graph,
    // place newcomers, and choose focus. A pointer selects the initial monitor.
    void adopt(
        std::vector<WindowObservation> const& windows,
        restart::Snapshot const* handoff,
        std::optional<std::pair<int16_t, int16_t>> pointer
    );
    // The candidate's placement must be valid; tiled clients join their workspace.
    void insert(Client client);
    // Dock reservations shape every monitor's workarea while the dock is registered.
    void insert_fixture(xcb_window_t id, Fixture::Role role, DockStrut strut = { });
    void reserve(xcb_window_t id, DockStrut strut);
    void erase(xcb_window_t id);

    // Configuration and topology
    // Layout, scratchpad slots and matching rules follow the installed
    // configuration; the workspace count is fixed once monitors exist.
    Config const& config() const { return config_; }
    std::expected<void, std::string> configure(Config config);
    // Rebind workspaces by output name and reassign clients. A changed topology
    // fits floating rectangles into the new workareas and clears monitor hints.
    void replace_topology(Topology topology);
    // EWMH desktops are monitor-major: monitor * workspaces + workspace.
    uint32_t desktop_index(size_t monitor, size_t workspace) const;
    // A concrete desktop on an existing monitor; the sticky value is not a placement.
    std::optional<std::pair<size_t, size_t>> desktop_placement(uint32_t desktop) const;

    // Derived views. Layout and visibility are never written back into clients.
    uint64_t revision() const { return revision_; }
    // Monotonic bounds for registration and completed focus ranks.
    uint64_t next_order() const { return next_order_; }
    uint64_t next_recency() const { return next_recency_; }
    bool in_view(Client const& client) const;
    std::vector<xcb_window_t> const& fullscreen_claims() const { return fullscreen_claims_; }
    std::vector<xcb_window_t> fullscreen_owners() const;
    xcb_window_t fullscreen_owner(size_t monitor) const;
    FullscreenVisibility fullscreen_visibility() const;
    bool visible(Client const& client) const;
    // Hot loops share fullscreen owners and descendant membership.
    bool visible(Client const& client, FullscreenVisibility const& fullscreen) const;
    // Another window owns fullscreen here and this client is not its descendant.
    bool suppressed(Client const& client) const;
    static bool accepts_focus(Client const& client) { return client.accepts_input || client.supports_take_focus; }
    bool focusable(Client const& client) const;
    bool focusable(Client const& client, FullscreenVisibility const& fullscreen) const;
    // Geometry: the model places frames; borders are drawn inside them.
    // A presentation is the X window rectangle and border inside a frame.
    struct Presentation
    {
        Geometry geometry;
        uint32_t border;
        bool operator==(Presentation const&) const = default;
    };
    // One immutable per-pass view per managed client, in registration order.
    // A presentation means visible; absence means hidden.
    struct Projected
    {
        Client const* client;
        std::optional<Presentation> presentation;
    };
    std::vector<Projected> project(FullscreenVisibility const& fullscreen) const;
    Presentation presentation(Client const& client) const;
    // The presented frame, and the normal frame that presentation states derive from.
    Geometry frame(Client const& client) const;
    Geometry normal_geometry(Client const& client) const;
    // Normal border width; fullscreen presentation has none.
    uint32_t border(Client const& client) const;
    uint32_t border_color(Client const& client) const;

    // Focus
    xcb_window_t active_window() const { return active_window_; }
    size_t focused_monitor() const { return focused_monitor_; }
    bool showing_desktop() const { return showing_desktop_; }
    // Activation restores a minimized client and selects its workspace. Unknown or
    // clients without input or WM_TAKE_FOCUS are refused; NONE releases focus
    // until a later choice, which settling does not override.
    void focus(xcb_window_t id, uint32_t time = 0, bool record_user_time = true);
    bool cycle_focus(bool forward);
    void restore(xcb_window_t id, bool activate);
    void focus_monitor(size_t monitor);
    // Focus the adjacent monitor's fallback; returns whether the monitor changed.
    bool focus_adjacent_monitor(int direction);
    // Outside drags, focus follows the pointer into visible clients; elsewhere
    // the pointer selects the monitor under it and releases focus there.
    void hover(xcb_window_t window, int16_t x, int16_t y);
    void show_desktop(bool enabled);
    // Settle one operation: end a drag whose context changed, then resolve
    // eligibility, recency and user time for the final focus. input_time is the
    // latest observed input timestamp. Returns the explicit focus request's time.
    std::optional<uint32_t> settle(uint32_t input_time = 0);

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
    // Move the active client to the adjacent monitor's current workspace.
    bool move_to_monitor(int direction);
    void floating(xcb_window_t id, bool enabled);
    // The user's float toggle focuses the client; it leaves fullscreen, minimized
    // and show-desktop presentation alone. Leaving floating also leaves maximize.
    void toggle_floating(xcb_window_t id);
    void geometry(xcb_window_t id, Geometry rectangle);
    void swap_tiles(size_t monitor, size_t a, size_t b);
    // Swap the focused monitor's current tile with the eligible tile `offset`
    // positions away; monocle focuses it instead, since every slot is shared.
    void swap_tile(int offset);

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
    void fullscreen_monitors(xcb_window_t id, std::optional<FullscreenMonitors> value);

    // Application and pager requests, already decoded by the shell
    // _NET_WM_STATE: read the complete request first; fullscreen dominates maximize.
    void request_states(xcb_window_t id, StateChange change, WindowStates requested);
    // _NET_ACTIVE_WINDOW. Application requests against another active client need a
    // timestamp no older than its user time; refusals demand attention. No request
    // surfaces a window suppressed by the fullscreen owner of its placement.
    void request_activation(xcb_window_t id, bool application, uint32_t timestamp);
    // _NET_WM_DESKTOP: a concrete desktop pins the placement; the sticky value sticks.
    void request_desktop(xcb_window_t id, uint32_t desktop);
    // _NET_CURRENT_DESKTOP names a monitor and one of its workspaces.
    void switch_desktop(uint32_t desktop);
    // An application's ConfigureRequest; tiled and fullscreen clients keep WM-owned geometry.
    void configure_request(xcb_window_t id, GeometryRequest request);
    // A pager's _NET_MOVERESIZE_WINDOW changes the normal floating rectangle.
    void moveresize_request(xcb_window_t id, GeometryRequest request);
    // WM_HINTS urgency is the application's own request; the active client has none.
    void hint_urgency(xcb_window_t id, bool urgent);

    // Metadata updates resolve classification defaults, parent placement,
    // changed rule actions and pending scratchpad claims.
    void title(xcb_window_t id, std::string value);
    void window_class(xcb_window_t id, std::string instance, std::string name);
    void window_type(xcb_window_t id, WindowType type);
    void transient(xcb_window_t id, xcb_window_t parent);
    void size_hints(xcb_window_t id, SizeHints hints, std::optional<Geometry> unmanaged_parent = std::nullopt);
    void focus_hints(xcb_window_t id, bool input, bool take_focus);
    // Activation-time bookkeeping is neither published nor a revision.
    void user_time(xcb_window_t source, uint32_t time);
    void user_time_window(xcb_window_t id, xcb_window_t window, uint32_t time);

    // Workspaces
    bool switch_workspace(size_t monitor, size_t workspace);
    // Focused-monitor workspace navigation; each returns the resulting workspace.
    size_t cycle_workspace(int step);
    size_t toggle_workspace();
    void layout(size_t monitor, LayoutStrategy strategy);
    void ratio(size_t monitor, SplitAddress address, double value);
    // The focused monitor's root split, within the configured ratio bounds.
    bool set_ratio(double value);
    bool adjust_ratio(double delta);
    void erase_ratio(size_t monitor, SplitAddress address);
    void reset_ratios(size_t monitor);

    // Scratchpads: the named slots and the pool are the only membership records.
    std::vector<NamedScratchpad> const& named_scratchpads() const { return named_scratchpads_; }
    std::vector<xcb_window_t> const& scratchpad_pool() const { return scratchpad_pool_; }
    NamedScratchpad const* named_scratchpad(std::string_view name) const;
    NamedScratchpad const* scratchpad_claim(xcb_window_t id) const;
    bool pooled(xcb_window_t id) const;
    void claim_scratchpad(xcb_window_t id, ScratchpadConfig const& config);
    // Returns the configuration whose command the shell should launch, if any.
    std::expected<ScratchpadConfig const*, std::string> toggle_scratchpad(std::string_view name);
    void scratchpad_pending(std::string_view name, bool pending);
    void pool_scratchpad(xcb_window_t id);
    // Hide the client in the pool. Claims, fullscreen, minimized and dragged
    // clients are left alone.
    void stash(xcb_window_t id);
    // Recall or focus the pool target; an active local target rotates the pool.
    void cycle_scratchpad_pool();

    // Pointer interactions. Domain changes start only after the shell acquires
    // the grab; a drag ends when its client or split context stops existing.
    std::optional<Drag> const& drag() const { return drag_; }
    // A button press on a window (NONE for bare root): mouse bindings, click to
    // focus, and gap clicks that resize or (double or Ctrl click) reset a split.
    Press press(xcb_window_t window, int16_t x, int16_t y, uint8_t button, uint16_t modifiers, uint32_t time);
    // _NET_WM_MOVERESIZE moves or resizes floating clients; cancel ends the same window's drag.
    std::optional<Interaction> moveresize(xcb_window_t id, floating::ResizeEdge edges) const;
    void cancel_moveresize(xcb_window_t id);
    void begin_drag(Interaction const& interaction, int16_t x, int16_t y, uint8_t button);
    std::optional<SplitHit> split_at(int16_t x, int16_t y) const;
    void drag_to(int16_t x, int16_t y);
    // Commit applies a tiled drop. Returns a committed split ratio change.
    std::optional<double> end_drag(bool commit);

    // Exec handoff
    restart::Snapshot snapshot() const;
    // Install a validated saved graph over freshly observed application state,
    // rebind it to discovered outputs, then admit newcomers in observation order.
    void restore_graph(restart::Snapshot const& snapshot, std::vector<Client> observed);

    // Publication reads a frozen model; Debug builds reject mutation meanwhile.
    void freeze() { frozen_ = true; }
    void thaw() { frozen_ = false; }

private:
    struct FocusRequest
    {
        uint32_t time;
        bool record_user_time;
    };

    Config config_;
    Geometry screen_;
    Clients clients_;
    Fixtures fixtures_;
    std::vector<Monitor> monitors_;
    std::vector<NamedScratchpad> named_scratchpads_;
    std::vector<xcb_window_t> scratchpad_pool_;
    std::vector<xcb_window_t> fullscreen_claims_;
    xcb_window_t active_window_ = XCB_NONE;
    size_t focused_monitor_ = 0;
    bool showing_desktop_ = false;
    std::optional<FocusRequest> focus_request_;
    std::vector<xcb_window_t> focus_cycle_;
    bool focus_released_ = false; ///< Focus was deliberately cleared, not left without a candidate
    std::optional<Drag> drag_;
    uint64_t next_order_ = 0;
    uint64_t next_recency_ = 1;
    uint64_t revision_ = 0;
    bool frozen_ = false;

    // Mutation bookkeeping
    void mutated();
    Client& edit(xcb_window_t id);
    Workspace& edit_workspace(size_t monitor, size_t workspace);
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
    uint64_t register_window(xcb_window_t id);
    void attach(Client const& client, std::optional<size_t> index = std::nullopt);
    std::optional<TileSlot> detach(Client const& client);

    // Admission, rules and metadata
    std::optional<Client> classify(WindowObservation const& window, restart::Snapshot const* handoff, bool adopting);
    void place(xcb_window_t id, std::optional<Geometry> unmanaged_parent);
    void apply_size_hints(xcb_window_t id, bool initial, std::optional<Geometry> unmanaged_parent);
    void apply_default_mode(xcb_window_t id);
    void apply_rule(xcb_window_t id, RuleActions const& rule);
    void apply_initial_rule(xcb_window_t id);
    bool match_rule(xcb_window_t id);
    void reapply_rules();
    void reconcile_metadata(xcb_window_t id);

    // Configuration and topology
    Monitor fresh_monitor(std::string name, Geometry geometry) const;
    void rebind(std::vector<Monitor> monitors);
    void update_workareas();
    void forget_missing_tile_slot(Client& client) const;

    // Views and geometry
    Layout layout() const { return { config_.appearance.padding, config_.layout }; }
    bool shows(size_t monitor, size_t workspace) const;
    // Eligible tiles of the current workspace, then sticky tiles of others.
    std::vector<xcb_window_t> tiled_participants(size_t monitor, FullscreenVisibility const& fullscreen) const;
    std::vector<xcb_window_t> workspace_tiles(
        size_t monitor, size_t workspace, FullscreenVisibility const* fullscreen, xcb_window_t include = XCB_NONE
    ) const;
    Geometry fullscreen_geometry(Client const& client) const;
    Presentation presentation(Client const& client, Geometry frame) const;
    void request_geometry(xcb_window_t id, Geometry rectangle);
    void set_mode(xcb_window_t id, bool floating);
    size_t wrap_monitor(int index) const;

    // Focus and client state
    void select_focus(xcb_window_t id, uint32_t time = 0, bool record_user_time = true);
    void focus_fallback(size_t monitor, bool record_user_time = true);
    std::optional<uint32_t> complete_focus(uint32_t input_time);
    void clear_urgency(xcb_window_t id);
    void pin_desktop(xcb_window_t id, bool pinned);

    // Scratchpads
    ScratchpadConfig const* match_scratchpad(Client const& client) const;
    template <typename Show> bool summon(xcb_window_t window, Show show);
    bool claim_pending_scratchpad(xcb_window_t id);
    void show_named_scratchpad(xcb_window_t id, ScratchpadConfig const& config);
    void show_pooled_scratchpad(xcb_window_t id);
    void advance_scratchpad_pool();
    void release_scratchpad(xcb_window_t id);
    void reconcile_scratchpads();

    // Pointer interactions
    struct GapClick
    {
        uint32_t time;
        SplitAddress address;
        size_t monitor;
    };
    std::optional<GapClick> gap_click_; ///< The last gap click, for double clicks
    bool can_drag(xcb_window_t id) const;
    bool drag_valid() const;
    std::optional<Geometry> drag_preview(Client const& client) const;
};

} // namespace lwm
