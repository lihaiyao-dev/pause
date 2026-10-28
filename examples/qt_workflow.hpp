#ifndef PAUSE_EXAMPLES_QT_WORKFLOW_HPP_
#define PAUSE_EXAMPLES_QT_WORKFLOW_HPP_
#include "qt_workflow_support.hpp"

namespace pause_graph {
// Business coroutines borrow the run context; the UI owns its lifetime.
inline stdexec::task<void> preview_step(RunContext& run, Branch branch, std::string key)
{
    co_await run.delay_step(branch, key);
}
inline stdexec::task<void> preview_long_path(RunContext& run, Branch branch)
{
    co_await run.delay_step(branch, "prepare");
    co_await run.delay_step(branch, "long-end");
}
// Replace this coroutine body with your business control flow.
inline stdexec::task<void> business(RunContext& run, Branch& root)
{
    co_await run.delay_step(root, "input");
    co_await run.group(root, "outer", 2, [&](auto& paths) {
        return stdexec::when_all(
            preview_step(run, std::move(paths[0]), "short"),
            preview_long_path(run, std::move(paths[1])));
    });
    co_await run.delay_step(root, "output");
}

// Display metadata is kept separate from the business coroutine.
inline Flow make_layout()
{
    return sequence({
        step("input", "入口"),
        parallel("outer", "并行路径", {
            step("short", "短路径"),
            sequence({step("prepare", "长路径：第一步"), step("long-end", "长路径：第二步")})}),
        step("output", "输出")});
}
inline Workflow make_workflow()
{
    return {make_layout(), business};
}
} // namespace pause_graph
#endif
