# Real Pause tests need execution.hpp and stop_token.hpp from one coherent tree:
# a renamed standalone pragma-once header would define stop types twice.
include("${CMAKE_CURRENT_LIST_DIR}/instrument_stop_token.cmake")
set(pause_instrumented_include "${CMAKE_CURRENT_BINARY_DIR}/pause-instrumented-stdexec")
file(COPY "${stdexec_SOURCE_DIR}/include/" DESTINATION "${pause_instrumented_include}")
# Reuse the existing hash-checked, insertion-count-checked instrumentation.
configure_file("${CMAKE_CURRENT_BINARY_DIR}/pause-test-hooks/pause_test_stop_token.hpp"
               "${pause_instrumented_include}/stdexec/stop_token.hpp" COPYONLY)
