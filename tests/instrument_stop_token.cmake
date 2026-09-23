# Instrument a build-only copy. Never change the pinned dependency checkout.
include_guard(GLOBAL)
set(pause_stop_header "${stdexec_SOURCE_DIR}/include/stdexec/stop_token.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pause_stop_header}")
file(SHA256 "${pause_stop_header}" pause_stop_hash)
if(NOT pause_stop_hash STREQUAL "243fb98e894c35cac231de20436f82c6a8de009c7178831fbc2c9c9536658bf3")
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
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pause-test-hooks/pause_test_stop_token.hpp" "${pause_stop_text}")
