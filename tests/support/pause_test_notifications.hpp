#pragma once

#include "pause.hpp"

namespace pause_test {
// One host signal can observe all three phase transitions. Same-phase commits
// deliberately produce no signal; callers that wait on counts must poll or use
// their own completion synchronization.
template<class Notify>
coop_pause::PausePhaseNotifications notifications(Notify notify)
{
    return {notify, notify, std::move(notify)};
}
} // namespace pause_test
