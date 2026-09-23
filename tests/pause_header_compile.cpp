#include "pause.hpp"

int pause_header_other();

int main()
{
    pause::PauseSource source(pause::AdvanceMode::Continuous, []() noexcept {});
    auto root = source.make_root_branch();
    if (!root || source.snapshot().live_branch_count != 1) return 1;
    return pause_header_other();
}
