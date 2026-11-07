#define PAUSE_ENABLE_INDEPENDENT_BRANCH 1
/* 测试项目：pause_resource_test
 * 性质: 真实 Pause 的逐分配点故障注入、回滚、资源计数与 connect 后无分配测试.
 * 测试项目:
 * - Continuous/SingleStep 创建根及 Continuous 创建独立分支: 失败返回 ResourceExhausted,
 *   快照与通知不变; 成功提交一次, 作用域退出后分配计数回到基线.
 * - 固定/动态四子分支分叉, 含嵌套父分支与两种模式: 逐个可恢复分配点失败,
 *   状态完整回滚; 每次失败后复用同一父分支重试成功, 防止隐藏的父记录损坏.
 * - 固定/动态零数量分叉, 含 moved-from 分支: 优先返回 InvalidForkCount 且不尝试分配.
 * - 超大动态分叉: 长度错误转换为 ResourceExhausted, 快照保持不变.
 * - 后续 task 创建失败: 未执行任何业务, 已创建的休眠兄弟 task 析构时释放其分支.
 * - 原始 sender 的 Open/Resume/Stop/QueuedStop, 含适用的空 token 路径:
 *   connect 完成后禁用分配仍正常完成, 分支析构不分配, 取消后参与计数归零.
 * - task 的四条相同路径: 禁用分配后仍完成宿主亲和投递, 仅 value 路径继续业务.
 * - MSVC checked iterator 专项: 动态分叉 vector 的三个 noexcept 代理分配点分别在
 *   两种模式下启动子进程, 必须在预定 bad_alloc 注入点 terminate 并退出 77.
 * 运行: ctest --test-dir <build> -R '^pause_resource_test$' --output-on-failure.
 * CLI: --iterations N (默认 100), 每轮 25 个状态族; --proxy-iterations N (默认 1)
 *      仅适用于 MSVC checked iterator 构建, 控制额外 6 类终止子进程. 次数范围 1..1000000.
 * 内部入口: --proxy-child INDEX continuous|step, 由父进程调用.
 * 边界: operator new/delete 仅在此测试程序替换, 故障注入仅作用于调用线程;
 *   无分配断言从操作已 connect 之后开始, 不宣称整个对象创建过程都无分配.
 */
#include "../support/pause_test_notifications.hpp"
#include "pause.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
#include <process.h>
#endif

// Failure injection is confined to this executable and the calling thread.
// No allocator changes or failure hooks are part of the production API.
namespace allocation_probe {
thread_local std::ptrdiff_t fail_at = -1;
thread_local std::size_t attempts = 0;
thread_local bool injected = false;
std::atomic<std::ptrdiff_t> outstanding{0};

void before_allocate()
{
    if (fail_at >= 0 && attempts++ == static_cast<std::size_t>(fail_at)) {
        injected = true;
        throw std::bad_alloc{};
    }
}

struct Failure {
    explicit Failure(std::size_t index) noexcept
    {
        attempts = 0;
        injected = false;
        fail_at = static_cast<std::ptrdiff_t>(index);
    }
    ~Failure() { fail_at = -1; }
};
} // namespace allocation_probe

void* operator new(std::size_t size)
{
    allocation_probe::before_allocate();
    if (void* result = std::malloc(size ? size : 1)) {
        ++allocation_probe::outstanding;
        return result;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept
{
    if (pointer) {
        --allocation_probe::outstanding;
        std::free(pointer);
    }
}
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

void* operator new(std::size_t size, std::align_val_t alignment)
{
    allocation_probe::before_allocate();
    const auto align = static_cast<std::size_t>(alignment);
#ifdef _WIN32
    void* result = _aligned_malloc(size ? size : 1, align);
#else
    void* result = nullptr;
    if (posix_memalign(&result, align, size ? size : 1) != 0) result = nullptr;
#endif
    if (!result) throw std::bad_alloc{};
    ++allocation_probe::outstanding;
    return result;
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{ return ::operator new(size, alignment); }
void operator delete(void* pointer, std::align_val_t) noexcept
{
    if (pointer) {
        --allocation_probe::outstanding;
#ifdef _WIN32
        _aligned_free(pointer);
#else
        std::free(pointer);
#endif
    }
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept
{ ::operator delete(pointer, alignment); }
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept
{ ::operator delete(pointer, alignment); }
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept
{ ::operator delete(pointer, alignment); }

namespace {
[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

constexpr std::size_t fork_child_count = 4;

template<bool Dynamic>
auto fork_children(coop_pause::PauseBranch& branch)
{
    if constexpr (Dynamic) return branch.fork_and_block_self(fork_child_count);
    else return branch.fork_and_block_self<fork_child_count>();
}

#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
// MSVC's checked-iterator proxy allocates even in vector's noexcept default
// and move constructors. These three failures terminate; the element buffer
// and all BranchRecord allocations still have to roll back normally.
constexpr std::size_t proxy_failure_indices[]{0, fork_child_count + 2, fork_child_count + 3};

bool is_proxy_failure(std::size_t index) noexcept
{
    for (auto proxy_index : proxy_failure_indices)
        if (index == proxy_index) return true;
    return false;
}

void check_vector_proxy_allocations()
{
    const auto initial_allocations = allocation_probe::outstanding.load();
    {
        allocation_probe::Failure count_only(
            static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()));
        std::vector<coop_pause::PauseBranch> original;
        CHECK(allocation_probe::attempts == 1);
        allocation_probe::attempts = 0;
        std::vector<coop_pause::PauseBranch> moved(std::move(original));
        CHECK(allocation_probe::attempts == 1);
    }
    CHECK(allocation_probe::outstanding == initial_allocations);
}

thread_local std::size_t proxy_failure_index = 0;
thread_local std::size_t proxy_notifications = 0;
thread_local std::size_t proxy_baseline_notifications = 0;

[[noreturn]] void proxy_failure_child(std::size_t index, coop_pause::AdvanceMode mode)
{
    CHECK(is_proxy_failure(index));
    coop_pause::PauseSource source(mode, pause_test::notifications([]() noexcept { ++proxy_notifications; }));
    auto root = source.make_root_branch();
    CHECK(root && proxy_notifications == (mode == coop_pause::AdvanceMode::SingleStep ? 1u : 0u));
    proxy_baseline_notifications = proxy_notifications;
    proxy_failure_index = index;
    std::set_terminate([] {
        const bool expected_failure = allocation_probe::injected &&
            allocation_probe::attempts == proxy_failure_index + 1 &&
            proxy_notifications == proxy_baseline_notifications;
        allocation_probe::fail_at = -1;
        auto exception = std::current_exception();
        if (!expected_failure || !exception) std::_Exit(78);
        try {
            std::rethrow_exception(exception);
        } catch (const std::bad_alloc&) {
            std::_Exit(77);
        } catch (...) {
            std::_Exit(79);
        }
    });
    allocation_probe::Failure failure(index);
    auto children = fork_children<true>(*root);
    (void)children;
    std::_Exit(80); // Recovering or returning normally is not a death-test pass.
}

void test_proxy_failures(const char* executable, int repetitions)
{
    check_vector_proxy_allocations();
    for (const char* mode : {"continuous", "step"}) {
        for (auto index : proxy_failure_indices) {
            char index_text[32];
            CHECK(std::snprintf(index_text, sizeof(index_text), "%zu", index) > 0);
            for (int iteration = 0; iteration != repetitions; ++iteration) {
                const char* arguments[]{executable, "--proxy-child", index_text, mode, nullptr};
                const auto code = _spawnv(_P_WAIT, executable, arguments);
                if (code != 77)
                    std::fprintf(stderr, "vector proxy failure %zu (%s) iteration %d exited %lld, expected 77\n",
                                 index, mode, iteration, static_cast<long long>(code));
                CHECK(code == 77);
            }
            std::printf("PASS proxy-termination=%zu mode=%s subprocesses=%d expected_exit=77\n",
                        index, mode, repetitions);
            std::fflush(stdout);
        }
    }
    std::printf("PASS: %d MSVC iterator-debug proxy allocation termination subprocesses\n", 6 * repetitions);
}
#endif

void same_snapshot(const coop_pause::PauseSnapshot& left, const coop_pause::PauseSnapshot& right)
{
    CHECK(left.revision == right.revision);
    CHECK(left.root_created == right.root_created);
    CHECK(left.phase == right.phase);
    CHECK(left.paused_epoch == right.paused_epoch);
    CHECK(left.origin == right.origin);
    CHECK(left.step_round == right.step_round);
    CHECK(left.waiting_branch_count == right.waiting_branch_count);
    CHECK(left.live_branch_count == right.live_branch_count);
}

void test_root(coop_pause::AdvanceMode mode)
{
    std::size_t failed_stages = 0;
    for (std::size_t index = 0; index != 32; ++index) {
        const auto initial_allocations = allocation_probe::outstanding.load();
        bool succeeded = false;
        {
            std::size_t notifications = 0;
            coop_pause::PauseSource source(mode, pause_test::notifications([&]() noexcept { ++notifications; }));
            const auto before = source.snapshot();
            {
                allocation_probe::Failure failure(index);
                auto root = source.make_root_branch();
                succeeded = root.has_value();
                if (!succeeded) {
                    CHECK(root.error() == coop_pause::PauseError::ResourceExhausted);
                    same_snapshot(source.snapshot(), before);
                    CHECK(notifications == 0);
                } else {
                    CHECK(source.snapshot().root_created);
                    CHECK(source.snapshot().live_branch_count == 1);
                    CHECK(notifications == (mode == coop_pause::AdvanceMode::SingleStep ? 1u : 0u));
                }
            }
            CHECK(source.snapshot().live_branch_count == 0);
        }
        CHECK(allocation_probe::outstanding == initial_allocations);
        if (succeeded) {
            CHECK(failed_stages > 0);
            return;
        }
        ++failed_stages;
    }
    CHECK(false);
}

void test_independent_branch()
{
    for (std::size_t index = 0; index != 32; ++index) {
        const auto initial_allocations = allocation_probe::outstanding.load();
        bool succeeded = false;
        {
            std::size_t notifications = 0;
            coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous, pause_test::notifications([&]() noexcept { ++notifications; }));
            auto root = source.make_root_branch();
            CHECK(root);
            const auto before = source.snapshot();
            const auto before_notifications = notifications;
            {
                allocation_probe::Failure failure(index);
                auto branch = source.make_branch();
                succeeded = branch.has_value();
                if (!succeeded) {
                    CHECK(branch.error() == coop_pause::PauseError::ResourceExhausted);
                    same_snapshot(source.snapshot(), before);
                    CHECK(notifications == before_notifications);
                } else {
                    CHECK(source.snapshot().live_branch_count == 2);
                    CHECK(notifications == before_notifications);
                }
            }
            CHECK(source.snapshot().live_branch_count == 1);
        }
        CHECK(allocation_probe::outstanding == initial_allocations);
        if (succeeded) { CHECK(index > 0); return; }
    }
    CHECK(false);
}

template<bool Dynamic>
void test_fork(coop_pause::AdvanceMode mode, bool nested = false)
{
    for (std::size_t index = 0; index != 64; ++index) {
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
        if constexpr (Dynamic) {
            // Each excluded index is independently required to terminate in
            // test_proxy_failures; no arbitrary allocation failure is skipped.
            if (is_proxy_failure(index)) continue;
        }
#endif
        const auto initial_allocations = allocation_probe::outstanding.load();
        bool succeeded = false;
        {
            std::size_t notifications = 0;
            coop_pause::PauseSource source(mode, pause_test::notifications([&]() noexcept { ++notifications; }));
            auto root = source.make_root_branch();
            CHECK(root);
            std::optional<std::array<coop_pause::PauseBranch, 1>> parent_holder;
            auto* parent = &*root;
            if (nested) {
                auto child = root->fork_and_block_self<1>();
                CHECK(child);
                parent_holder.emplace(std::move(*child));
                parent = &(*parent_holder)[0];
            }
            const auto before = source.snapshot();
            const auto before_notifications = notifications;
            {
                allocation_probe::Failure failure(index);
                auto children = fork_children<Dynamic>(*parent);
                succeeded = children.has_value();
                if (!succeeded) {
                    CHECK(children.error() == coop_pause::PauseError::ResourceExhausted);
                    same_snapshot(source.snapshot(), before);
                    CHECK(notifications == before_notifications);
                } else {
                    CHECK(children->size() == fork_child_count);
                    CHECK(source.snapshot().live_branch_count == fork_child_count);
                    CHECK(notifications == before_notifications);
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
                    if constexpr (Dynamic) {
                        CHECK(index == fork_child_count + 4);
                        CHECK(allocation_probe::attempts == index);
                    }
#endif
                }
            }
            if (!succeeded) {
                // A matching snapshot alone would miss a poisoned parent
                // record. Reuse the same parent after every recoverable stage.
                auto retry = fork_children<Dynamic>(*parent);
                CHECK(retry && retry->size() == fork_child_count);
                CHECK(source.snapshot().live_branch_count == fork_child_count);
                CHECK(notifications == before_notifications);
            }
            CHECK(source.snapshot().live_branch_count == 1);
            if (mode == coop_pause::AdvanceMode::SingleStep)
                CHECK(source.snapshot().phase == coop_pause::GatePhase::Collecting);
        }
        CHECK(allocation_probe::outstanding == initial_allocations);
        if (succeeded) { CHECK(index >= fork_child_count); return; }
    }
    CHECK(false);
}

void test_zero_count_before_allocation(coop_pause::AdvanceMode mode)
{
    const auto initial_allocations = allocation_probe::outstanding.load();
    {
        std::size_t notifications = 0;
        coop_pause::PauseSource source(mode, pause_test::notifications([&]() noexcept { ++notifications; }));
        auto root = source.make_root_branch();
        CHECK(root);
        coop_pause::PauseBranch moved(std::move(*root));
        const auto before = source.snapshot();
        const auto before_notifications = notifications;
        {
            allocation_probe::Failure failure(0);
            for (auto* branch : {&moved, &*root}) {
                auto fixed = branch->fork_and_block_self<0>();
                auto dynamic = branch->fork_and_block_self(0);
                CHECK(!fixed && fixed.error() == coop_pause::PauseError::InvalidForkCount);
                CHECK(!dynamic && dynamic.error() == coop_pause::PauseError::InvalidForkCount);
            }
            CHECK(allocation_probe::attempts == 0 && !allocation_probe::injected);
            same_snapshot(source.snapshot(), before);
            CHECK(notifications == before_notifications);
        }
    }
    CHECK(allocation_probe::outstanding == initial_allocations);
}

void test_fork_length_error()
{
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous);
    auto root = source.make_root_branch();
    CHECK(root);
    const auto before = source.snapshot();
    auto children = root->fork_and_block_self(std::numeric_limits<std::size_t>::max());
    CHECK(!children && children.error() == coop_pause::PauseError::ResourceExhausted);
    same_snapshot(source.snapshot(), before);
}

stdexec::task<void> dormant_task(coop_pause::PauseBranch branch, bool& entered)
{
    co_await branch.suspend_point();
    entered = true;
}

void test_task_creation_failure()
{
    const auto initial_allocations = allocation_probe::outstanding.load();
    {
        coop_pause::PauseSource source(coop_pause::AdvanceMode::SingleStep);
        auto root = source.make_root_branch();
        CHECK(root);
        auto children = root->fork_and_block_self<4>();
        CHECK(children);
        bool entered = false;
        {
            // Already-created dormant siblings must also release their owned
            // branches when creation of a later task throws.
            auto first = dormant_task(std::move((*children)[0]), entered);
            auto second = dormant_task(std::move((*children)[1]), entered);
            bool threw = false;
            {
                allocation_probe::Failure failure(0);
                try {
                    auto rejected = dormant_task(std::move((*children)[2]), entered);
                    (void)rejected;
                } catch (const std::bad_alloc&) { threw = true; }
            }
            CHECK(threw && !entered);
            CHECK(source.snapshot().live_branch_count == 3);
        }
        CHECK(source.snapshot().live_branch_count == 1);
    }
    CHECK(allocation_probe::outstanding == initial_allocations);
}

struct Receiver {
    using receiver_concept = stdexec::receiver_tag;
    stdexec::run_loop* loop;
    stdexec::inplace_stop_token token;
    int* result;
    auto get_env() const noexcept
    {
        return stdexec::env{
            stdexec::prop{stdexec::get_start_scheduler, loop->get_scheduler()},
            stdexec::prop{stdexec::get_stop_token, token}};
    }
    void set_value() noexcept { CHECK(*result == 0); *result = 1; loop->finish(); }
    void set_stopped() noexcept { CHECK(*result == 0); *result = 2; loop->finish(); }
    template<class Error>
    void set_error(Error&&) noexcept { fail("unexpected public error", __LINE__); }
};

struct LaunchReceiver {
    using receiver_concept = stdexec::receiver_tag;
    stdexec::env<> get_env() const noexcept { return {}; }
    void set_value() noexcept {}
    void set_stopped() noexcept { fail("uncancellable launch stopped", __LINE__); }
    template<class Error>
    void set_error(Error&&) noexcept { fail("launch error", __LINE__); }
};

// All operations are prepared before allocation is disabled. The launch sender
// establishes the actual run_loop context declared by the Pause receiver.
void test_no_allocation_after_connect(int route, bool empty_token)
{
    stdexec::run_loop loop;
    stdexec::inplace_stop_source stop;
    coop_pause::PauseSource source(route == 0 ? coop_pause::AdvanceMode::Continuous
                                        : coop_pause::AdvanceMode::SingleStep);
    std::optional<coop_pause::PauseBranch> branch;
    {
        auto root = source.make_root_branch();
        CHECK(root);
        branch.emplace(std::move(*root));
    }
    int result = 0;
    {
        auto operation = stdexec::connect(branch->suspend_point(), Receiver{
            &loop, empty_token ? stdexec::inplace_stop_token{} : stop.get_token(), &result});
        auto launch = stdexec::connect(
            stdexec::schedule(loop.get_scheduler()) | stdexec::then([&]() noexcept {
                stdexec::start(operation);
                if (route == 0) { CHECK(result == 1); return; }
                CHECK(result == 0);
                CHECK(source.snapshot().phase == coop_pause::GatePhase::Paused);
                if (route == 2) {
                    CHECK(stop.request_stop());
                    CHECK(result == 2);
                } else {
                    CHECK(source.resume(*source.snapshot().paused_epoch,
                                        coop_pause::AdvanceMode::SingleStep));
                    CHECK(result == 0);
                    if (route == 3) CHECK(stop.request_stop());
                }
            }), LaunchReceiver{});
        allocation_probe::Failure failure(0);
        stdexec::start(launch);
        loop.run();
        CHECK(result == ((route == 2 || route == 3) ? 2 : 1));
        CHECK(allocation_probe::attempts == 0);
        if (route >= 2) {
            CHECK(source.snapshot().live_branch_count == 0);
            CHECK(source.snapshot().phase == coop_pause::GatePhase::Paused);
        }
    }
    {
        allocation_probe::Failure failure(0);
        branch.reset();
        CHECK(source.snapshot().live_branch_count == 0);
        CHECK(allocation_probe::attempts == 0);
    }
}

void test_task_no_allocation_after_connect(int route)
{
    stdexec::run_loop loop;
    stdexec::inplace_stop_source stop;
    coop_pause::PauseSource source(route == 0 ? coop_pause::AdvanceMode::Continuous
                                        : coop_pause::AdvanceMode::SingleStep);
    auto root = source.make_root_branch();
    CHECK(root);
    bool continued = false;
    int result = 0;
    {
        auto operation = stdexec::connect(
            stdexec::starts_on(loop.get_scheduler(),
                              dormant_task(std::move(*root), continued)),
            Receiver{&loop, stop.get_token(), &result});
        auto control = stdexec::connect(
            stdexec::schedule(loop.get_scheduler()) | stdexec::then([&]() noexcept {
                if (route == 0) { CHECK(result == 1); return; }
                CHECK(result == 0);
                CHECK(source.snapshot().phase == coop_pause::GatePhase::Paused);
                if (route == 2) {
                    CHECK(stop.request_stop());
                    // Default task's affinity adapter must deliver later;
                    // responsibility has already been withdrawn here.
                    CHECK(result == 0 && source.snapshot().live_branch_count == 0);
                } else {
                    CHECK(source.resume(*source.snapshot().paused_epoch,
                                        coop_pause::AdvanceMode::SingleStep));
                    CHECK(result == 0);
                    if (route == 3) CHECK(stop.request_stop());
                }
            }), LaunchReceiver{});
        allocation_probe::Failure failure(0);
        stdexec::start(operation);
        stdexec::start(control);
        loop.run();
        CHECK(result == ((route == 2 || route == 3) ? 2 : 1));
        CHECK(continued == (result == 1));
        CHECK(source.snapshot().live_branch_count == 0);
        CHECK(allocation_probe::attempts == 0);
    }
    CHECK(source.snapshot().live_branch_count == 0);
}

void run_cases()
{
    for (auto mode : {coop_pause::AdvanceMode::Continuous, coop_pause::AdvanceMode::SingleStep}) {
        test_root(mode);
        test_fork<false>(mode);
        test_fork<true>(mode);
        test_fork<false>(mode, true);
        test_fork<true>(mode, true);
        test_zero_count_before_allocation(mode);
    }
    test_independent_branch();
    test_fork_length_error();
    test_task_creation_failure();
    for (int route = 0; route != 4; ++route) test_no_allocation_after_connect(route, false);
    test_no_allocation_after_connect(0, true);
    test_no_allocation_after_connect(1, true);
    for (int route = 0; route != 4; ++route) test_task_no_allocation_after_connect(route);
}
} // namespace

int main(int argc, char** argv)
{
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
    if (argc == 4 && std::strcmp(argv[1], "--proxy-child") == 0) {
        char* end = nullptr;
        const long index = std::strtol(argv[2], &end, 10);
        CHECK(end != argv[2] && *end == '\0' && index >= 0);
        const bool single_step = std::strcmp(argv[3], "step") == 0;
        CHECK(single_step || std::strcmp(argv[3], "continuous") == 0);
        proxy_failure_child(static_cast<std::size_t>(index), single_step
            ? coop_pause::AdvanceMode::SingleStep : coop_pause::AdvanceMode::Continuous);
    }
#endif
    int iterations = 100, proxy_iterations = 1;
    CHECK(argc % 2 == 1);
    for (int index = 1; index < argc; index += 2) {
        char* end = nullptr;
        const long count = std::strtol(argv[index + 1], &end, 10);
        CHECK(end != argv[index + 1] && *end == '\0' && count > 0 && count <= 1000000);
        if (std::strcmp(argv[index], "--iterations") == 0) iterations = static_cast<int>(count);
        else {
            CHECK(std::strcmp(argv[index], "--proxy-iterations") == 0);
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
            proxy_iterations = static_cast<int>(count);
#else
            CHECK(false); // This configuration cannot exercise iterator proxy termination.
#endif
        }
    }
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
    test_proxy_failures(argv[0], proxy_iterations);
#endif
    for (int i = 0; i != iterations; ++i) run_cases();
    std::printf("Pause resource rollback and no-allocation paths: %d cases passed (%d repetitions)\n",
                iterations * 25, iterations);
    std::printf("resource coverage: 25 state families, each repeated %d times; "
                "every recoverable fork allocation stage retried on the same parent\n", iterations);
}
