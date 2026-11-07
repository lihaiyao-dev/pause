/* 测试项目：stdexec_standard_task_api_test
 * 目的: 核对固定版本新 stdexec::task 的公开 API、环境和 frame 资源行为。
 * 测试项:
 * 1. 编译期 sender/move-only 属性、默认 token/scheduler 类型和 scheduler 接受条件。
 * 2. move-only 返回值与引用身份; awaited error 可捕获, stopped 绕过 catch 且不继续协程。
 * 3. typed co_yield error 经 sender 连接保留原始 int 通道及载荷 23。
 * 4. 默认 task 不透传任意自定义查询; 显式 TaskEnv 保存查询值 73。
 * 5. std::stop_token 桥接为 inplace_stop_token 后仍可观察预先请求的停止。
 * 6. value/error/stopped 三路径 frame 分配释放平衡, 未启动 task 移动后析构,
 *    创建时分配失败抛出 bad_alloc 且协程未进入。
 * 预期: 返回值、控制流和固定依赖差异符合断言, 已分配字节最终为 0, 无重复释放。
 * 入口: 配置 PAUSE_BUILD_DESIGN_TESTS=ON, CTest 名称 stdexec_standard_task_api_test。
 *       CLI --iterations N, 默认 100, 范围 1..1000000, 每轮 13 个运行期用例。
 * 边界: 不含 pause.hpp; frame allocator 与执行 allocator 是不同资源。
 *       ThrowingConnectScheduler 仅做类型接受性检查, 不执行依赖中会进入 unreachable 的路径。
 */
#include <stdexec/execution.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

// Public-API/resource probes for the pinned stdexec::task, not a Pause test.
namespace {

[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

static_assert(stdexec::sender<stdexec::task<void>>);
static_assert(!std::is_copy_constructible_v<stdexec::task<void>>);
static_assert(std::is_nothrow_move_constructible_v<stdexec::task<void>>);
static_assert(std::is_same_v<stdexec::task<>::stop_token_type,
                             stdexec::inplace_stop_token>);
static_assert(std::is_same_v<stdexec::task<>::start_scheduler_type,
                             stdexec::task_scheduler>);
static_assert(!stdexec::sender_in<stdexec::task<void>, stdexec::env<>>);

// A valid scheduler is not necessarily accepted by default task_scheduler.
template<bool AdvertiseError, bool ThrowOnConnect = false>
struct ExampleScheduler {
    struct Sender {
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = std::conditional_t<AdvertiseError,
            stdexec::completion_signatures<stdexec::set_value_t(),
                                          stdexec::set_error_t(std::exception_ptr)>,
            stdexec::completion_signatures<stdexec::set_value_t()>>;
        struct Attrs {
            ExampleScheduler query(stdexec::get_completion_scheduler_t<
                                   stdexec::set_value_t>) const noexcept { return {}; }
        };
        Attrs get_env() const noexcept { return {}; }
        template<class Receiver>
        struct Operation {
            Receiver receiver;
            void start() & noexcept { stdexec::set_value(std::move(receiver)); }
        };
        template<class Receiver>
        auto connect(Receiver receiver) const noexcept(!ThrowOnConnect)
        {
            if constexpr (ThrowOnConnect) throw std::bad_alloc{};
            return Operation<Receiver>{std::move(receiver)};
        }
    };
    Sender schedule() const noexcept { return {}; }
    bool operator==(const ExampleScheduler&) const = default;
};
using FallibleScheduler = ExampleScheduler<true>;
static_assert(stdexec::scheduler<FallibleScheduler>);
static_assert(!std::is_constructible_v<stdexec::task_scheduler, FallibleScheduler>);
// Conversely, omitting error completions does not make connect non-throwing.
// The pinned task_scheduler accepts this type, but its backend catch enters
// unreachable if connect throws. Do not execute that undefined-behavior path.
using ThrowingConnectScheduler = ExampleScheduler<false, true>;
static_assert(stdexec::scheduler<ThrowingConnectScheduler>);
static_assert(std::is_constructible_v<stdexec::task_scheduler, ThrowingConnectScheduler>);

stdexec::task<std::unique_ptr<int>> move_only_result()
{
    co_return std::make_unique<int>(42);
}

stdexec::task<int&> reference_result(int& value)
{
    co_return value;
}

stdexec::task<int> catch_awaited_error()
{
    try {
        co_await stdexec::just_error(std::runtime_error("expected sender error"));
    } catch (const std::runtime_error&) {
        co_return 17;
    }
    fail("error must throw at co_await", __LINE__);
}

stdexec::task<void> stopped_bypasses_catch(bool& caught, bool& continued)
{
    try {
        co_await stdexec::just_stopped();
    } catch (...) {
        caught = true;
    }
    continued = true;
}

struct TypedErrors {
    using error_types = stdexec::completion_signatures<
        stdexec::set_error_t(int), stdexec::set_error_t(std::exception_ptr)>;
};

stdexec::task<void, TypedErrors> typed_error(bool& continued)
{
    co_yield stdexec::with_error{23};
    continued = true;
}

struct CustomQuery : stdexec::forwarding_query_t {
    template<class Env>
        requires requires(const Env& env, const CustomQuery& query) { env.query(query); }
    int operator()(const Env& env) const noexcept { return env.query(*this); }
};

stdexec::task<bool> default_environment_has_custom_query()
{
    co_return co_await stdexec::read_env([](const auto& env) noexcept {
        return std::is_invocable_v<CustomQuery, decltype(env)>;
    });
}

struct CustomTaskEnv {
    int value;

    template<class Env>
        requires std::is_invocable_v<CustomQuery, const Env&>
    explicit CustomTaskEnv(const Env& env) noexcept : value(CustomQuery{}(env)) {}

    int query(CustomQuery) const noexcept { return value; }
};

stdexec::task<int, CustomTaskEnv> custom_environment_query()
{
    co_return co_await stdexec::read_env(CustomQuery{});
}

stdexec::task<bool> translated_stop_token()
{
    const auto token = co_await stdexec::read_env(stdexec::get_stop_token);
    static_assert(std::is_same_v<decltype(token), const stdexec::inplace_stop_token>);
    co_return token.stop_requested();
}

struct AllocationState {
    std::size_t bytes = 0;
    int allocated = 0;
    int deallocated = 0;
    int entered = 0;
    bool reject = false;
};

template<class T>
struct TrackedAllocator {
    using value_type = T;
    AllocationState* state;

    explicit TrackedAllocator(AllocationState& value) noexcept : state(&value) {}
    template<class U>
    TrackedAllocator(const TrackedAllocator<U>& other) noexcept : state(other.state) {}

    T* allocate(std::size_t count)
    {
        if (state->reject) throw std::bad_alloc{};
        auto* result = std::allocator<T>{}.allocate(count);
        state->bytes += count * sizeof(T);
        ++state->allocated;
        return result;
    }
    void deallocate(T* pointer, std::size_t count) noexcept
    {
        state->bytes -= count * sizeof(T);
        ++state->deallocated;
        std::allocator<T>{}.deallocate(pointer, count);
    }
    bool operator==(const TrackedAllocator&) const = default;
};

// Frame allocation is explicitly supplied at coroutine creation. The task's
// execution allocator remains its default; these are distinct resources.
stdexec::task<void> allocated_task(std::allocator_arg_t,
                                  TrackedAllocator<std::byte>,
                                  AllocationState& state, int outcome)
{
    ++state.entered;
    if (outcome == 1) co_await stdexec::just_stopped();
    if (outcome == 2) throw std::runtime_error("expected allocated task error");
    co_return;
}

void test_allocation(int outcome)
{
    AllocationState state;
    bool error = false;
    try {
        auto task = allocated_task(std::allocator_arg,
                                   TrackedAllocator<std::byte>{state}, state, outcome);
        CHECK(state.bytes > 0 && state.entered == 0);
        auto result = stdexec::sync_wait(std::move(task));
        CHECK(result.has_value() == (outcome == 0));
    } catch (const std::runtime_error&) {
        error = true;
    }
    CHECK(error == (outcome == 2));
    CHECK(state.entered == 1);
    CHECK(state.bytes == 0);
    CHECK(state.allocated == state.deallocated && state.allocated > 0);
}

void test_dormant_and_failed_allocation()
{
    AllocationState state;
    {
        auto task = allocated_task(std::allocator_arg,
                                   TrackedAllocator<std::byte>{state}, state, 0);
        auto owner = std::move(task);
        CHECK(state.bytes > 0 && state.entered == 0);
    }
    CHECK(state.bytes == 0 && state.entered == 0);
    CHECK(state.allocated == state.deallocated);

    state.reject = true;
    bool threw = false;
    try {
        auto task = allocated_task(std::allocator_arg,
                                   TrackedAllocator<std::byte>{state}, state, 0);
        (void)task;
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw && state.bytes == 0 && state.entered == 0);
    CHECK(state.allocated == state.deallocated);
}

void run_cases()
{
    auto move_result = stdexec::sync_wait(move_only_result());
    CHECK(move_result && *std::get<0>(*move_result) == 42);

    int value = 31;
    auto reference = stdexec::sync_wait(reference_result(value)
        | stdexec::then([](int& item) { return std::ref(item); }));
    CHECK(reference && &std::get<0>(*reference).get() == &value);

    auto caught_error = stdexec::sync_wait(catch_awaited_error());
    CHECK(caught_error && std::get<0>(*caught_error) == 17);

    bool caught = false, continued = false;
    CHECK(!stdexec::sync_wait(stopped_bypasses_catch(caught, continued)));
    CHECK(!caught && !continued);

    int error_value = 0;
    bool error_erased = false;
    CHECK(stdexec::sync_wait(typed_error(continued)
        | stdexec::upon_error([&](auto error) noexcept {
              if constexpr (std::is_same_v<decltype(error), int>) error_value = error;
              else if constexpr (std::is_same_v<decltype(error), std::exception_ptr>) {
                  // An unexpected erasure must still preserve its payload;
                  // the baseline below requires the typed channel directly.
                  error_erased = true;
                  try { std::rethrow_exception(error); }
                  catch (int item) { error_value = item; }
                  catch (...) { fail("unexpected exception payload", __LINE__); }
              } else fail("unexpected typed task error", __LINE__);
          })).has_value());
    CHECK(error_value == 23 && !error_erased && !continued);

    auto property = stdexec::prop{CustomQuery{}, 73};
    auto missing = stdexec::sync_wait(default_environment_has_custom_query()
        | stdexec::write_env(property));
    CHECK(missing && !std::get<0>(*missing));
    auto copied = stdexec::sync_wait(custom_environment_query()
        | stdexec::write_env(property));
    CHECK(copied && std::get<0>(*copied) == 73);

    std::stop_source source;
    source.request_stop();
    auto bridged = stdexec::sync_wait(translated_stop_token()
        | stdexec::write_env(stdexec::prop{stdexec::get_stop_token, source.get_token()}));
    CHECK(bridged && std::get<0>(*bridged));

    for (int outcome = 0; outcome != 3; ++outcome) test_allocation(outcome);
    test_dormant_and_failed_allocation();
}

} // namespace

int main(int argc, char** argv)
{
    int iterations = 100;
    if (argc != 1) {
        CHECK(argc == 3 && std::strcmp(argv[1], "--iterations") == 0);
        char* end = nullptr;
        const long parsed = std::strtol(argv[2], &end, 10);
        CHECK(end != argv[2] && *end == '\0' && parsed > 0 && parsed <= 1000000);
        iterations = static_cast<int>(parsed);
    }
    for (int i = 0; i != iterations; ++i) run_cases();
    std::printf("stdexec::task API/resources: %d cases passed (%d repetitions)\n",
                iterations * 13, iterations);
    std::puts("Pinned observations: default task omits custom queries; configured env preserves them;");
    std::puts("typed co_yield error preserves its declared error channel through sender connection.");
}
