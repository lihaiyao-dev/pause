#include <pause.hpp>

int main()
{
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous, []() noexcept {});
    auto root = source.make_root_branch();
    if (!root) return 1;
    const auto state = source.snapshot();
    return state.root_created && state.phase == coop_pause::GatePhase::Open ? 0 : 2;
}
