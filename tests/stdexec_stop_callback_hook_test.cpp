#include <atomic>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace pause_test_hooks {
void after_registration() noexcept;
void after_selection() noexcept;
void inside_remove_wait() noexcept;
}

// Generated from the exact pinned header. Only this executable uses the copy.
#include "pause_test_stop_token.hpp"

namespace {
[[noreturn]] void fail(const char* expression, int line) {
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

struct HookState {
    bool hold_registration = false;
    bool hold_selection = false;
    std::latch registered{1}, release_registration{1};
    std::latch selected{1}, release_selection{1};
    std::latch removing{1};
    std::atomic<bool> removing_reported{false};
};
// Installed before starting threads; cleared only after all threads are joined.
HookState* hooks = nullptr;

enum class Phase { Registering, CancelRequested, Waiting, Value, Stopped };
struct Control {
    std::mutex mutex;
    std::latch decided{1}, tail_entered{1}, release_tail{1}, completed{1};
    std::latch release_completion{1};
    std::atomic<int> values{0}, stops{0}, destroyed{0}, resets{0}, callbacks{0};
    bool destroy_in_completion = false;
    bool hold_start_tail = false;
    bool hold_completion_tail = false;
    bool open = false;
};

// Executes the document's raw operation phase/lifetime protocol with a real
// callback. No Pause records, intrusive list, scheduler or public sender exists.
struct Operation {
    struct StopFn {
        Operation* operation;
        void operator()() noexcept { operation->on_stop(); }
    };
    Control* control;
    Phase phase = Phase::Registering;
    stdexec::inplace_stop_token token;
    std::optional<stdexec::inplace_stop_callback<StopFn>> callback;

    Operation(Control& current, stdexec::inplace_stop_token current_token)
        : control(&current), token(current_token) {}
    ~Operation() {
        // Explicitly finish unregistering before publishing destruction. In
        // production the callback is the last member for the same ordering.
        callback.reset();
        ++control->destroyed;
    }
    void complete(bool value) noexcept {
        auto* local = control;
        if (value) ++local->values;
        else ++local->stops;
        if (local->destroy_in_completion) delete this;
        local->completed.count_down();
        if (local->hold_completion_tail) local->release_completion.wait();
    }
    void start() noexcept {
        auto* local = control;
        callback.emplace(token, StopFn{this});
        bool owns = false, value = false;
        {
            std::lock_guard lock(local->mutex);
            if (phase == Phase::CancelRequested) {
                phase = Phase::Stopped;
                owns = true;
            } else if (local->open) {
                phase = Phase::Value;
                owns = value = true;
            } else {
                CHECK(phase == Phase::Registering);
                phase = Phase::Waiting;
            }
        }
        local->decided.count_down();
        if (local->hold_start_tail) {
            local->tail_entered.count_down();
            local->release_tail.wait();
        }
        if (!owns) return; // Operation may have been destroyed in the tail.
        ++local->resets;
        callback.reset();
        complete(value);
    }
    void on_stop() noexcept {
        auto* local = control;
        ++local->callbacks;
        {
            std::lock_guard lock(local->mutex);
            if (phase == Phase::Registering) {
                phase = Phase::CancelRequested;
                return;
            }
            if (phase != Phase::Waiting) return;
            phase = Phase::Stopped;
        }
        complete(false); // May destroy StopFn and this. No subsequent access.
    }
};

void registered_before_constructor_returns() {
    HookState state;
    state.hold_registration = true;
    hooks = &state;
    stdexec::inplace_stop_source source;
    Control control;
    Operation operation(control, source.get_token());
    std::thread starter([&] { operation.start(); });
    state.registered.wait();
    CHECK(!source.request_stop());
    {
        std::lock_guard lock(control.mutex);
        CHECK(operation.phase == Phase::CancelRequested);
    }
    CHECK(control.callbacks == 1 && control.stops == 0 && control.resets == 0);
    state.release_registration.count_down();
    starter.join();
    CHECK(control.stops == 1 && control.values == 0 && control.resets == 1);
    hooks = nullptr;
}

void selected_during_construction(bool open) {
    HookState state;
    state.hold_registration = state.hold_selection = true;
    hooks = &state;
    stdexec::inplace_stop_source source;
    Control control;
    control.open = open;
    Operation operation(control, source.get_token());
    std::thread starter([&] { operation.start(); });
    state.registered.wait();
    std::thread requester([&] { CHECK(!source.request_stop()); });
    state.selected.wait();
    CHECK(control.callbacks == 0);
    state.release_registration.count_down();
    control.decided.wait();
    {
        std::lock_guard lock(control.mutex);
        CHECK(operation.phase == (open ? Phase::Value : Phase::Waiting));
    }
    if (open) {
        // Coordinator is separate from the blocked unregistering thread.
        state.removing.wait();
        CHECK(control.values == 0 && control.callbacks == 0);
    }
    state.release_selection.count_down();
    starter.join();
    requester.join();
    CHECK(control.callbacks == 1);
    CHECK(control.values == (open ? 1 : 0));
    CHECK(control.stops == (open ? 0 : 1));
    CHECK(control.resets == (open ? 1 : 0));
    hooks = nullptr;
}

void selected_before_user_function_unregister() {
    HookState state;
    state.hold_selection = true;
    hooks = &state;
    stdexec::inplace_stop_source source;
    Control control;
    auto* operation = new Operation(control, source.get_token());
    operation->start();
    std::thread requester([&] { CHECK(!source.request_stop()); });
    state.selected.wait();
    {
        std::lock_guard lock(control.mutex);
        operation->phase = Phase::Value; // Resume wins before callback enters.
    }
    std::thread resumer([&] {
        ++control.resets;
        operation->callback.reset();
        operation->complete(true);
    });
    state.removing.wait(); // Actual library wait-loop entered, not a timing guess.
    CHECK(control.callbacks == 0 && control.values == 0);
    state.release_selection.count_down();
    requester.join();
    resumer.join();
    CHECK(control.callbacks == 1 && control.values == 1 && control.stops == 0);
    delete operation;
    CHECK(control.destroyed == 1);
    hooks = nullptr;
}

void completion_destroys_operation_before_start_returns(bool stopped) {
    HookState state;
    hooks = &state;
    stdexec::inplace_stop_source source;
    Control control;
    control.hold_start_tail = control.destroy_in_completion = true;
    auto* operation = new Operation(control, source.get_token());
    std::thread starter([operation] { operation->start(); });
    control.tail_entered.wait();
    if (stopped) {
        CHECK(!source.request_stop()); // Callback's completion destroys itself.
    } else {
        {
            std::lock_guard lock(control.mutex);
            CHECK(operation->phase == Phase::Waiting);
            operation->phase = Phase::Value;
        }
        ++control.resets;
        operation->callback.reset();
        operation->complete(true);
    }
    CHECK(control.destroyed == 1);
    CHECK(control.values == (stopped ? 0 : 1));
    CHECK(control.stops == (stopped ? 1 : 0));
    control.release_tail.count_down();
    starter.join();
    hooks = nullptr;
}

void completed_operation_destroyed_on_another_thread() {
    HookState state;
    hooks = &state;
    stdexec::inplace_stop_source source;
    Control control;
    control.hold_completion_tail = true;
    auto* operation = new Operation(control, source.get_token());
    operation->start();
    std::thread requester([&] { CHECK(!source.request_stop()); });
    control.completed.wait();
    std::thread destroyer([operation] { delete operation; });
    state.removing.wait();
    CHECK(control.stops == 1 && control.destroyed == 0);
    // The coordinator releases the notifier; the notifier never joins its
    // destroyer. This avoids the wait cycle prohibited by section 7.6.3.
    control.release_completion.count_down();
    requester.join();
    destroyer.join();
    CHECK(control.destroyed == 1 && control.resets == 0);
    hooks = nullptr;
}

int iterations(int argc, char** argv) {
    if (argc == 1) return 100;
    CHECK(argc == 3 && std::string_view(argv[1]) == "--iterations");
    std::string_view text(argv[2]);
    int count = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    CHECK(error == std::errc{} && end == text.data() + text.size());
    CHECK(count > 0 && count <= 100000);
    return count;
}
}

namespace pause_test_hooks {
void after_registration() noexcept {
    if (hooks && hooks->hold_registration) {
        hooks->registered.count_down();
        hooks->release_registration.wait();
    }
}
void after_selection() noexcept {
    if (hooks && hooks->hold_selection) {
        hooks->selected.count_down();
        hooks->release_selection.wait();
    }
}
void inside_remove_wait() noexcept {
    if (hooks && !hooks->removing_reported.exchange(true))
        hooks->removing.count_down();
}
}

int main(int argc, char** argv) {
    const int count = iterations(argc, argv);
    for (int i = 0; i != count; ++i) {
        registered_before_constructor_returns();
        selected_during_construction(true);
        selected_during_construction(false);
        selected_before_user_function_unregister();
        completion_destroys_operation_before_start_returns(true);
        completion_destroys_operation_before_start_returns(false);
        completed_operation_destroyed_on_another_thread();
    }
    std::cout << "PASS: 7 deterministic callback/lifetime cases x " << count
              << " iterations (instrumented pinned dependency and raw-protocol probe only)\n";
}
