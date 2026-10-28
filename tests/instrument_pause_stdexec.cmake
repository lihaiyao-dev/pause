# 测试辅助项目：为真实 Pause sender 竞态测试生成完整且一致的 stdexec 插桩副本。
# 复用 instrument_stop_token.cmake 的哈希与插入次数校验，替换构建目录副本的 stop_token。
# 预期配置成功并仅改 build 下副本；生产依赖不变。本文件没有独立运行断言。
# Real Pause tests need execution.hpp and stop_token.hpp from one coherent tree:
# a renamed standalone pragma-once header would define stop types twice.
include("${CMAKE_CURRENT_LIST_DIR}/instrument_stop_token.cmake")
set(pause_instrumented_include "${CMAKE_CURRENT_BINARY_DIR}/pause-instrumented-stdexec")
file(COPY "${pause_stdexec_include_dir}/" DESTINATION "${pause_instrumented_include}")
# Reuse the existing hash-checked, insertion-count-checked instrumentation.
configure_file("${CMAKE_CURRENT_BINARY_DIR}/pause-test-hooks/pause_test_stop_token.hpp"
               "${pause_instrumented_include}/stdexec/stop_token.hpp" COPYONLY)
