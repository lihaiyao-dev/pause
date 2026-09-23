#include "pause.hpp"

#include <stop_token>
#include <utility>

// PAUSE_ENVIRONMENT_CASE=0 is the positive control. Cases 1 and 2 must be
// rejected at connect even if the Pause gate would take the Open path.
#ifndef PAUSE_ENVIRONMENT_CASE
#define PAUSE_ENVIRONMENT_CASE 0
#endif

struct CompileReceiver {
    using receiver_concept = stdexec::receiver_tag;
    stdexec::run_loop* loop;
    stdexec::inplace_stop_source* stop;
    auto get_env() const noexcept
    {
#if PAUSE_ENVIRONMENT_CASE == 1
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, stop->get_token()}};
#elif PAUSE_ENVIRONMENT_CASE == 2
        return stdexec::env{
            stdexec::prop{stdexec::get_start_scheduler, loop->get_scheduler()},
            stdexec::prop{stdexec::get_stop_token, std::stop_token{}}};
#else
        return stdexec::env{
            stdexec::prop{stdexec::get_start_scheduler, loop->get_scheduler()},
            stdexec::prop{stdexec::get_stop_token, stop->get_token()}};
#endif
    }
    void set_value() noexcept {}
    void set_stopped() noexcept {}
    template<class Error> void set_error(Error&&) noexcept {}
};

void pause_environment_compile(pause::PauseBranch& branch,
                               stdexec::run_loop& loop,
                               stdexec::inplace_stop_source& stop)
{
    auto operation = stdexec::connect(branch.suspend_point(), CompileReceiver{&loop, &stop});
    (void)operation;
}
