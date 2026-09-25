/* 测试项目：task_scheduler 内联存储布局探针
 * 性质: 固定 stdexec 内部类型的编译期布局核验与大小观测, 不是 Pause 运行期压力.
 * 项目: 比较 stoppable/unstoppable 调度 operation 的 sizeof 与 alignof, 检查均适配
 * STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE/ALIGN, 并断言 run_loop schedule 为 noexcept.
 * 预期: static_assert 均成立; main 可构造/复制 task_scheduler 并输出实际布局数值.
 * 运行: 同目录 build.cmd 编译后运行 build/pause-audit-20260925/layout-probe/layout.exe.
 * 边界: 依赖固定版本私有 __task 类型, 结论仅适用于当次编译器、ABI 与宏配置.
 */
#include "pause.hpp"
#include <cstdio>
using Scheduler = stdexec::run_loop::scheduler;
using Sender = stdexec::schedule_result_t<Scheduler>;
using Allocator = std::allocator<Scheduler>;
using Unstoppable = stdexec::__task::__opstate<Allocator, Sender, stdexec::__task::__env_t<true>>;
using Stoppable = stdexec::__task::__opstate<Allocator, Sender, stdexec::__task::__env_t<false>>;
static_assert(sizeof(Stoppable) == sizeof(Unstoppable));
static_assert(alignof(Stoppable) == alignof(Unstoppable));
static_assert(sizeof(Stoppable) <= STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE);
static_assert(alignof(Stoppable) <= STDEXEC_TASK_SCHEDULE_OPSTATE_ALIGN);
static_assert(noexcept(stdexec::schedule(std::declval<Scheduler>())));
int main() {
  stdexec::run_loop loop;
  stdexec::task_scheduler scheduler{loop.get_scheduler()};
  auto copy = scheduler;
  (void)copy;
  std::printf("stoppable_size=%zu unstoppable_size=%zu align=%zu storage_size=%d storage_align=%d task_scheduler_size=%zu\n", sizeof(Stoppable), sizeof(Unstoppable), alignof(Stoppable), STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE, STDEXEC_TASK_SCHEDULE_OPSTATE_ALIGN, sizeof(stdexec::task_scheduler));
}
