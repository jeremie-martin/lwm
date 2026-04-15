#pragma once

#include "split_tree.hpp"
#include <cstddef>

namespace lwm {

// LayoutStrategy enum is defined in types.hpp

/// Build a master-stack tree: first window is master (left), rest stack vertically (right).
/// Default ratios: root H-split = 0.5, stack V-splits produce equal distribution.
TreeNode build_master_stack(size_t window_count, double default_ratio = 0.5);

TreeNode build_layout_tree(LayoutStrategy strategy, size_t window_count, double default_ratio = 0.5);

} // namespace lwm
