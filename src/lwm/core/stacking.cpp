#include "lwm/core/stacking.hpp"
#include "lwm/core/classification.hpp"
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <tuple>

namespace lwm::stacking {
namespace {
enum class Tier
{
    Below,
    Normal,
    Above,
    Fullscreen
};

struct Entry
{
    xcb_window_t id;
    bool visible;
    Tier tier;
    bool floating;
    uint64_t order;
    xcb_window_t transient_for;
};
}

std::vector<xcb_window_t> compute_order(State const& state)
{
    auto owners = state.fullscreen_owners();
    xcb_window_t active = state.active_window();
    std::vector<Entry> ranked;
    ranked.reserve(state.clients().size() + state.fixtures().size());
    std::vector<xcb_window_t> result;
    result.reserve(ranked.capacity());
    for (auto const& [id, fixture] : state.fixtures())
        ranked.push_back(
            { id, true, fixture.role == Fixture::Role::Desktop ? Tier::Below : Tier::Above, false, fixture.order, XCB_NONE }
        );
    bool has_transients = false;
    for (auto const& [id, client] : state.clients())
    {
        bool in_view = state.in_view(client);
        auto owner = owners[client.monitor];
        bool suppressed = in_view && owner != XCB_NONE && owner != id && owner != client.transient_for;
        auto layer = effective_layer(client);
        Tier tier = suppressed        ? Tier::Below
            : client.fullscreen       ? Tier::Fullscreen
            : layer == LayerHint::Above ? Tier::Above
            : layer == LayerHint::Below ? Tier::Below
                                        : Tier::Normal;
        bool floating = client.kind() == Client::Kind::Floating;
        bool visible = in_view && !suppressed;
        has_transients |= visible && floating && client.transient_for != XCB_NONE;
        ranked.push_back({ id, visible, tier, floating, client.order, client.transient_for });
    }
    auto key = [active](Entry const& value)
    { return std::tuple{ value.visible, value.tier, value.floating, value.id == active, value.order, value.id }; };
    std::sort(ranked.begin(), ranked.end(), [&](auto const& a, auto const& b) { return key(a) < key(b); });
    if (!has_transients)
    {
        for (auto const& value : ranked) result.push_back(value.id);
        return result;
    }

    // Each client has at most one parent. Indices retain base stacking priority.
    size_t const none = ranked.size();
    struct Links
    {
        size_t parent, child, sibling, visited;
    };
    std::vector<Links> links(ranked.size(), { none, none, none, none });
    std::unordered_map<xcb_window_t, size_t> positions;
    positions.reserve(ranked.size());
    for (size_t i = 0; i < ranked.size(); ++i) positions.emplace(ranked[i].id, i);
    for (size_t i = 0; i < ranked.size(); ++i)
    {
        auto const& value = ranked[i];
        auto parent = positions.find(value.transient_for);
        if (value.visible && value.floating && parent != positions.end() && ranked[parent->second].visible)
            links[i].parent = parent->second;
    }
    // Functional graph: each node has at most one parent. A walk marked with
    // its starting index detects only its own cycle; previous walks are done.
    // Cut one edge per cycle before ordering so unrelated tiers retain priority.
    for (size_t start = 0; start < links.size(); ++start)
    {
        size_t node = start;
        while (node != none && links[node].visited == none)
        {
            links[node].visited = start;
            node = links[node].parent;
        }
        if (node == none || links[node].visited != start)
            continue;
        size_t cut = node;
        for (size_t i = links[node].parent; i != node; i = links[i].parent) cut = std::min(cut, i);
        links[cut].parent = none;
    }
    std::vector<size_t> ready;
    ready.reserve(ranked.size());
    for (size_t i = 0; i < links.size(); ++i)
    {
        size_t parent = links[i].parent;
        if (parent == none)
            ready.push_back(i);
        else
        {
            links[i].sibling = links[parent].child;
            links[parent].child = i;
        }
    }
    std::make_heap(ready.begin(), ready.end(), std::greater<>{ });
    while (!ready.empty())
    {
        std::pop_heap(ready.begin(), ready.end(), std::greater<>{ });
        size_t current = ready.back();
        ready.pop_back();
        result.push_back(ranked[current].id);
        for (size_t child = links[current].child; child != none; child = links[child].sibling)
        {
            ready.push_back(child);
            std::push_heap(ready.begin(), ready.end(), std::greater<>{ });
        }
    }
    return result;
}

// Keep a longest subsequence already in server order. Each other
// managed window needs one move; unrelated root children are not reordered.
std::vector<StackMove>
plan_moves(std::span<xcb_window_t const> server_order, std::span<xcb_window_t const> desired_order)
{
    // The common unchanged order is a subsequence of the root children. Check
    // it before allocating lookup tables; unrelated children need no handling.
    size_t matched = 0;
    for (auto window : server_order)
        if (matched < desired_order.size() && window == desired_order[matched])
            ++matched;
    if (matched == desired_order.size())
        return { };

    std::unordered_map<xcb_window_t, size_t> positions;
    positions.reserve(server_order.size());
    for (size_t i = 0; i < server_order.size(); ++i) positions.emplace(server_order[i], i);
    std::vector<xcb_window_t> windows;
    std::vector<size_t> ranks;
    for (auto window : desired_order)
    {
        auto found = positions.find(window);
        // A client may have been destroyed since the policy was computed.
        if (found == positions.end())
            continue;
        windows.push_back(window);
        ranks.push_back(found->second);
    }
    if (std::is_sorted(ranks.begin(), ranks.end()))
        return { };
    size_t none = windows.size();
    std::vector<size_t> tails, previous(windows.size(), none);
    for (size_t i = 0; i < windows.size(); ++i)
    {
        auto tail = std::lower_bound(
            tails.begin(),
            tails.end(),
            ranks[i],
            [&](size_t index, size_t rank) { return ranks[index] < rank; }
        );
        if (tail != tails.begin())
            previous[i] = *(tail - 1);
        if (tail == tails.end())
            tails.push_back(i);
        else
            *tail = i;
    }
    std::vector<bool> keep(windows.size());
    size_t anchor = tails.back();
    for (size_t i = anchor; i != none; i = previous[i])
    {
        keep[i] = true;
        anchor = i;
    }
    std::vector<StackMove> moves;
    moves.reserve(windows.size() - tails.size());
    for (size_t i = 0; i < windows.size(); ++i)
        if (!keep[i])
            moves.push_back(
                { windows[i], i ? windows[i - 1] : windows[anchor], i ? XCB_STACK_MODE_ABOVE : XCB_STACK_MODE_BELOW }
            );
    return moves;
}

} // namespace lwm::stacking
