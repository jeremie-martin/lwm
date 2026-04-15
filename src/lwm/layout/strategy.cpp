#include "strategy.hpp"

namespace lwm {

TreeNode build_master_stack(size_t window_count, double default_ratio)
{
    if (window_count <= 1)
        return make_leaf(0);

    if (window_count == 2)
        return make_split(SplitDirection::Horizontal, default_ratio, make_leaf(0), make_leaf(1));

    // 3+ windows: master on left, right-leaning vertical chain for stack.
    // Stack ratios give equal distribution: split i gets ratio = 1.0 / (K - i).
    size_t stack_count = window_count - 1;

    TreeNode stack = make_leaf(window_count - 1);
    for (size_t i = stack_count - 1; i >= 1; --i)
    {
        size_t remaining = stack_count - (i - 1);
        double ratio = 1.0 / static_cast<double>(remaining);
        stack = make_split(SplitDirection::Vertical, ratio, make_leaf(i), std::move(stack));
    }

    return make_split(SplitDirection::Horizontal, default_ratio, make_leaf(0), std::move(stack));
}

TreeNode build_layout_tree(LayoutStrategy strategy, size_t window_count, double default_ratio)
{
    switch (strategy)
    {
    case LayoutStrategy::MasterStack:
        return build_master_stack(window_count, default_ratio);
    }
    return build_master_stack(window_count, default_ratio);
}

} // namespace lwm
