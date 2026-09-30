#pragma once
#include "events.hpp"
#include "restart.hpp"
#include "types.hpp"
#include <map>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>

namespace lwm {
// The only writer of managed domain records. Transport and policy readers get
// const views; changes and their completion obligations are recorded together.
class State
{
public:
    struct TransitionEffects
    {
        std::map<size_t, xcb_window_t> monitors;
        std::vector<xcb_window_t> geometry;
        std::set<xcb_window_t> states;
        std::set<size_t> layouts;
        std::set<xcb_window_t> configure_replies;
        std::vector<xcb_window_t> maps;
        std::map<xcb_window_t, bool> visibility;
        std::optional<xcb_window_t> previous_focus;
        uint32_t focus_time = XCB_CURRENT_TIME;
        bool drain_crossing = false;
        bool repair_focus = false;
        bool state_changed = false;
        bool stacking = false;
        bool workareas = false;
        bool desktop_metadata = false;
        bool appearance = false;
        bool workarea_property = false;
        bool showing_desktop = false;
        std::set<xcb_window_t> fullscreen_properties;
        bool client_list = false;
        bool current_desktop = false;
        std::set<xcb_window_t> allowed_actions;
        std::set<xcb_window_t> urgency;
        std::set<xcb_window_t> desktops;
        std::set<xcb_window_t> iconic;
        std::map<size_t, std::pair<size_t, size_t>> workspace_events;
        std::vector<std::pair<EventType, std::string>> events;
        bool operator==(TransitionEffects const&) const = default;
    };

    struct NamedScratchpadState
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
            if (auto const* claimed = std::get_if<Claimed>(&state))
                return claimed->window;
            return XCB_NONE;
        }

        bool pending_launch() const { return std::holds_alternative<LaunchPending>(state); }

        void mark_empty() { state = Empty{ }; }

        void mark_launch_pending() { state = LaunchPending{ }; }

        void mark_claimed(xcb_window_t claimed_window) { state = Claimed{ claimed_window }; }
    };
    std::vector<NamedScratchpadState> const& named_scratchpads() const { return named_scratchpads_; }
    std::vector<xcb_window_t> const& scratchpad_pool() const { return scratchpad_pool_; }
    void configure_scratchpads(std::span<std::string const> names);
    void scratchpad_pending(std::string_view name, bool pending = true);
    void restore_pool(std::span<xcb_window_t const> ids);

    using Clients = std::unordered_map<xcb_window_t, Client>;
    Clients const& clients() const { return clients_; }
    std::vector<Monitor> const& monitors() const { return monitors_; }
    Client const* find(xcb_window_t id) const;
    Client const& require(xcb_window_t id) const;

    xcb_window_t const& active_window() const { return active_window_; }
    size_t const& focused_monitor() const { return focused_monitor_; }
    bool const& showing_desktop() const { return showing_desktop_; }
    void focus(xcb_window_t id, uint32_t time = 0);
    void focus_monitor(size_t monitor);
    void showing_desktop(bool enabled);
    void restore_focus(size_t monitor, xcb_window_t id, bool desktop);
    void insert(Client client);
    void erase(xcb_window_t id);
    enum class RelocationGeometry
    {
        Preserve,
        CenterOnMonitorChange
    };
    bool relocate(
        xcb_window_t id,
        size_t monitor,
        size_t workspace,
        RelocationGeometry geometry = RelocationGeometry::Preserve,
        std::optional<size_t> index = std::nullopt
    );
    void change_kind(xcb_window_t id, ClientState state, std::optional<size_t> index = std::nullopt);
    void geometry(xcb_window_t id, Geometry rectangle);
    void tiled_geometry(xcb_window_t id, Geometry rectangle);
    void iconic(xcb_window_t id, bool enabled);
    void sticky(xcb_window_t id, bool enabled);
    void borderless(xcb_window_t id, bool enabled);
    void floating_preference(xcb_window_t id, bool enabled);
    void layer(xcb_window_t id, LayerHint hint);
    void maximize(xcb_window_t id, bool horizontal, bool vertical);
    void modal(xcb_window_t id, bool enabled);
    void fullscreen(xcb_window_t id, bool enabled);
    void skip_taskbar(xcb_window_t id, bool enabled);
    void skip_pager(xcb_window_t id, bool enabled);
    void urgency(xcb_window_t id, UrgencySource source, bool enabled);
    void clear_urgency(xcb_window_t id);
    void scratchpad(xcb_window_t id, std::optional<ScratchpadMembership> membership);
    void pin_desktop(xcb_window_t id, bool pinned);
    void configure_suppression(xcb_window_t id, bool enabled);
    void title(xcb_window_t id, std::string value);
    void window_class(xcb_window_t id, std::string instance, std::string name);
    void window_type(xcb_window_t id, WindowType type);
    void transient(xcb_window_t id, xcb_window_t parent);
    void focus_hints(xcb_window_t id, bool input, bool take_focus);
    void user_time(xcb_window_t id, uint32_t time, xcb_window_t window);
    void fullscreen_monitors(xcb_window_t id, std::optional<FullscreenMonitors> value);
    void remember_focus(size_t monitor, size_t workspace, xcb_window_t id);
    bool switch_workspace(size_t monitor, size_t workspace);
    void layout(size_t monitor, LayoutStrategy strategy);
    void ratio(size_t monitor, SplitAddress address, double value);
    void erase_ratio(size_t monitor, SplitAddress address);
    void reset_ratios(size_t monitor);
    void swap_tiles(size_t monitor, size_t a, size_t b);
    void invalidate(size_t monitor, xcb_window_t preferred = XCB_NONE);
    void request_geometry(xcb_window_t id);
    void resolve_owner(size_t monitor, xcb_window_t owner);
    void workarea(size_t monitor, Strut strut);
    void fit_floating();
    void replace_monitors(std::vector<Monitor> monitors);
    void restore_workspaces(size_t monitor, size_t current, size_t previous);
    void restore_layout(size_t monitor, size_t workspace, LayoutStrategy strategy, SplitRatioMap ratios);
    void restore_tile_order(std::span<xcb_window_t const> order);
    void restore_client(xcb_window_t id, restart::ClientRecord const& record);
    uint64_t next_registration() const { return next_client_order_; }
    uint64_t next_recency() const { return next_mru_order_; }
    void touch(xcb_window_t id);
    // Output acknowledgements affect only X bookkeeping, never domain policy.
    ClientPresentation& presentation(xcb_window_t id);

    // The coordinator consumes obligations and adds transport-only work. This
    // API never grants writable access to managed records or workspace graphs.
    TransitionEffects& effects() { return effects_; }
    TransitionEffects begin_publication();
    void end_publication();

private:
    bool publishing_ = false;
    xcb_window_t active_window_ = XCB_NONE;
    size_t focused_monitor_ = 0;
    bool showing_desktop_ = false;
    std::set<xcb_window_t> displaced_;
    std::vector<NamedScratchpadState> named_scratchpads_;
    std::vector<xcb_window_t> scratchpad_pool_;
    Clients clients_;
    std::vector<Monitor> monitors_;
    TransitionEffects effects_;
    uint64_t next_client_order_ = 0;
    uint64_t next_mru_order_ = 0;
    Client& edit(xcb_window_t id);
    void classification(xcb_window_t id, bool update_mode = false);
    void attach(Client const& client, std::optional<size_t> index = std::nullopt);
    std::optional<SavedTilePos> detach(Client const& client);
    void changed(xcb_window_t id);
};
}
