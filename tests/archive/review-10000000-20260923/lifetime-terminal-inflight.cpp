/* 测试项目：lifetime-terminal-inflight (已并入正式测试的历史原型)
 * 测试项: 原 callback 的 terminal CPO 仍执行时, 分别由协调线程或最终 receiver 同步销毁外层 operation。
 * 预期: 原 operation 在 callback 内退役; 宿主交付 stopped 恰好一次; 完整外层销毁后尾部只访问外部状态。
 * 历史入口: 同目录 lifetime-run.cmd, 每轮两个场景, --iterations 默认沿用被包含测试的 100。
 * 边界: 真实 callback/let_stopped/run_loop 依赖组合, 不含 pause.hpp。
 * 本文件扩展的 InflightControl/StartTailGate/terminal_inflight 已纳入当前正式源码;
 * 如今直接包含正式源码会重复定义, 此快照只用于历史审计, 不作为当前可编译压力入口。
 * 当前验证请运行 tests/stdexec_callback_affine_lifetime_test.cpp 的 CTest 目标。
 */
#define main existing_callback_affine_main
#include "../../stdexec_callback_affine_lifetime_test.cpp"
#undef main

// Extend the existing pinned-composition probe with a gate immediately after
// the real run_loop schedule has started, before its initiating start returns.
// All gate storage outlives both the operation and the requesting thread.
namespace {
struct InflightControl : Control {
    std::latch schedule_started{1};
    std::latch release_schedule_start{1};
    std::atomic<int> outer_destroyed{0};
    bool synchronous_destruction = false;
    void* outer = nullptr;
    void (*destroy_outer)(InflightControl*) noexcept = nullptr;
};

template<class Inner>
struct StartTailGate {
    using sender_concept = stdexec::sender_tag;
    Inner inner;
    InflightControl* control;

    template<class Env>
    auto get_completion_signatures(Env&&) const
        -> stdexec::completion_signatures_of_t<Inner, Env> { return {}; }

    template<class Receiver>
    struct Operation {
        InflightControl* control;
        stdexec::connect_result_t<Inner, Receiver> inner;

        Operation(Inner sender, Receiver receiver, InflightControl* current)
            : control(current), inner(stdexec::connect(std::move(sender), std::move(receiver))) {}

        void start() & noexcept {
            auto* local = control;
            stdexec::start(inner);
            local->schedule_started.count_down();
            local->release_schedule_start.wait();
            // This gate, its inner schedule and the complete enclosing let chain
            // have been destroyed. Do not access any operation member here.
            CHECK(local->outer_destroyed == 1);
        }
    };

    template<class Receiver>
    auto connect(Receiver receiver) && {
        return Operation<Receiver>{std::move(inner), std::move(receiver), control};
    }
};

struct InflightReceiver {
    using receiver_concept = stdexec::receiver_tag;
    InflightControl* control;
    auto get_env() const noexcept {
        return stdexec::env{
            stdexec::prop{stdexec::get_stop_token, control->source.get_token()},
            stdexec::prop{stdexec::get_start_scheduler, control->loop.get_scheduler()}};
    }
    void set_stopped() noexcept {
        auto* local = control;
        CHECK(local->raw_destroyed == 1 && local->after_terminal == 0);
        CHECK(local->callback_returning == 0 && local->request_returned == 0);
        CHECK(++local->stopped == 1);
        local->completion_thread = std::this_thread::get_id();
        local->loop.finish();
        if (local->synchronous_destruction) local->destroy_outer(local);
        local->completed.count_down();
        // This receiver may already have been destroyed above or by coordinator.
    }
    template<class Error>
    void set_error(Error&&) noexcept { CHECK(false); }
};

auto inflight_sender(InflightControl& control) {
    return stdexec::let_stopped(CallbackSender{&control}, [local = &control] {
        return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
            [local](auto scheduler) {
                ++local->schedules;
                return stdexec::let_value(
                    StartTailGate{stdexec::unstoppable(stdexec::schedule(scheduler)), local},
                    [local] {
                        CHECK(local->source.stop_requested());
                        ++local->schedule_values;
                        return stdexec::just_stopped();
                    });
            });
    });
}

void terminal_inflight(bool synchronous_destruction) {
    InflightControl control;
    control.synchronous_destruction = synchronous_destruction;
    using Operation = decltype(stdexec::connect(inflight_sender(control), InflightReceiver{&control}));
    control.outer = new Operation(stdexec::connect(inflight_sender(control), InflightReceiver{&control}));
    control.destroy_outer = [](InflightControl* local) noexcept {
        delete static_cast<Operation*>(local->outer);
        local->outer = nullptr;
        ++local->outer_destroyed;
    };
    stdexec::start(*static_cast<Operation*>(control.outer));
    std::thread requester([&] {
        CHECK(!control.source.request_stop());
        ++control.request_returned;
    });
    control.schedule_started.wait();
    CHECK(control.raw_destroyed == 1 && control.after_terminal == 0);
    std::thread worker([&] {
        control.scheduler_thread = std::this_thread::get_id();
        control.loop.run();
    });
    control.completed.wait();
    if (!synchronous_destruction) control.destroy_outer(&control);
    CHECK(control.outer_destroyed == 1 && control.request_returned == 0);
    CHECK(control.after_terminal == 0 && control.callback_returning == 0);
    control.release_schedule_start.count_down();
    requester.join();
    worker.join();
    check_finished(control);
}
}

int main(int argc, char** argv) {
    const int count = iterations(argc, argv);
    for (int index = 0; index != count; ++index) {
        terminal_inflight(false);
        terminal_inflight(true);
    }
    std::cout << "PASS: 2 in-flight terminal destruction cases x " << count
              << " iterations (real callback/let_stopped/run_loop composition, not Pause)\n";
}
