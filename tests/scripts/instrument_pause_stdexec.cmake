# 测试辅助项目：在固定 stdexec stop_token 的注册、选中回调及删除等待点插入测试 hook。
# 先校验上游 SHA256，再要求每个 hook 恰好插入一次；不匹配时配置必须失败。
# 仅生成构建目录中的测试头，供可控竞态测试使用；不修改上游源码或提供运行时验收。
# Instrument a build-only copy. Never change the pinned dependency checkout.
include_guard(GLOBAL)
set(pause_stop_header "${pause_stdexec_include_dir}/stdexec/stop_token.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pause_stop_header}")
file(SHA256 "${pause_stop_header}" pause_stop_hash)
if(NOT pause_stop_hash STREQUAL "e3587cea5441c36f1cd181c3974522ce104ff81b3b5d1de4cd6418a88695d228")
    message(FATAL_ERROR "The stop-token test hooks require the reviewed pinned header; re-audit before updating its hash")
endif()
file(READ "${pause_stop_header}" pause_stop_text)
string(REPLACE "      __register_callback_();"
               "      __register_callback_();\n      ::pause_test_hooks::after_registration();"
               pause_stop_text "${pause_stop_text}")
string(REPLACE "      __callbk->__execute();"
               "      ::pause_test_hooks::after_selection();\n      __callbk->__execute();"
               pause_stop_text "${pause_stop_text}")
string(REPLACE "          __spin.__wait();\n        }\n      }\n    }\n  }"
               "          ::pause_test_hooks::inside_remove_wait();\n          __spin.__wait();\n        }\n      }\n    }\n  }"
               pause_stop_text "${pause_stop_text}")
foreach(pause_hook IN ITEMS after_registration after_selection inside_remove_wait)
    string(REGEX MATCHALL "pause_test_hooks::${pause_hook}" pause_matches "${pause_stop_text}")
    list(LENGTH pause_matches pause_match_count)
    if(NOT pause_match_count EQUAL 1)
        message(FATAL_ERROR "Expected exactly one insertion of ${pause_hook}")
    endif()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pause-test-hooks")
set(pause_hook_header "${CMAKE_CURRENT_BINARY_DIR}/pause-test-hooks/pause_test_stop_token.hpp")
set(pause_previous_stop_text "")
if(EXISTS "${pause_hook_header}")
    file(READ "${pause_hook_header}" pause_previous_stop_text)
endif()
if(NOT pause_stop_text STREQUAL pause_previous_stop_text)
    file(WRITE "${pause_hook_header}" "${pause_stop_text}")
endif()

# Real Pause tests need execution.hpp and stop_token.hpp from one coherent tree:
# a renamed standalone pragma-once header would define stop types twice.
# Design-only tests use the standalone hook header generated above.
if(PAUSE_BUILD_TESTS)
    set(pause_instrumented_include "${CMAKE_CURRENT_BINARY_DIR}/pause-instrumented-stdexec")
    file(COPY "${pause_stdexec_include_dir}/" DESTINATION "${pause_instrumented_include}"
        PATTERN "stop_token.hpp" EXCLUDE)
    configure_file("${pause_hook_header}"
        "${pause_instrumented_include}/stdexec/stop_token.hpp" COPYONLY)
endif()
