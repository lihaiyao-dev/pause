#include "pause.hpp"

// This translation unit deliberately emits the same inline public definitions
// as pause_header_compile.cpp, so linking catches header ODR violations.
int pause_header_other()
{
    pause::PauseSource source(pause::AdvanceMode::Continuous, []() noexcept {});
    auto root = source.make_root_branch();
    if (!root) return 1;
    auto children = root->fork_and_block_self<2>();
    if (!children || source.snapshot().live_branch_count != 2) return 2;
    return 0;
}
