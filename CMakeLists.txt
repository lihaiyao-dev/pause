cmake_minimum_required(VERSION 3.25)

project(pause LANGUAGES CXX)

include(FetchContent)

set(STDEXEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(STDEXEC_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    stdexec
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG f0e8ae6fdc6c188389b146bb854b80d399724b04
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(stdexec)

add_library(pause INTERFACE)
add_library(pause::pause ALIAS pause)

target_compile_features(pause INTERFACE cxx_std_23)
target_include_directories(
    pause
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(pause INTERFACE STDEXEC::stdexec)
if(MSVC)
    # Resource rollback in the public header requires C++ stack unwinding.
    target_compile_options(pause INTERFACE /EHsc)
endif()

option(PAUSE_BUILD_TOOLCHAIN_SMOKE_TEST
       "Build a small target that verifies the stdexec toolchain"
       ON)

if(PAUSE_BUILD_TOOLCHAIN_SMOKE_TEST)
    add_executable(pause_toolchain_smoke tests/toolchain_smoke.cpp)
    target_link_libraries(pause_toolchain_smoke PRIVATE pause::pause)

    add_executable(pause_header_compile tests/pause_header_compile.cpp tests/pause_header_other.cpp)
    target_link_libraries(pause_header_compile PRIVATE pause::pause)
endif()

option(PAUSE_BUILD_TESTS "Build the real Pause implementation acceptance tests" ON)

if(PAUSE_BUILD_TESTS)
    enable_testing()
    include(tests/instrument_pause_stdexec.cmake)
    foreach(test_name IN ITEMS pause_integration_test pause_sender_test
                               pause_sender_hook_test pause_sender_death_test pause_resource_test
                               pause_task_lifetime_test pause_extreme_test pause_batch_extreme_test
                               pause_transition_extreme_test)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE pause::pause)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
    endforeach()
    target_include_directories(pause_sender_hook_test BEFORE PRIVATE "${pause_instrumented_include}")
    if(PAUSE_BUILD_TOOLCHAIN_SMOKE_TEST)
        add_test(NAME pause_header_compile COMMAND pause_header_compile)
        add_test(NAME pause_toolchain_smoke COMMAND pause_toolchain_smoke)
    endif()
    # The positive/negative compile probes must use the same configuration and
    # CRT as the executable tests, including Debug iterator diagnostics.
    set(pause_environment_configurations ${CMAKE_CONFIGURATION_TYPES} "${CMAKE_BUILD_TYPE}")
    list(REMOVE_DUPLICATES pause_environment_configurations)
    set(pause_environment_config_flags "")
    foreach(configuration IN LISTS pause_environment_configurations)
        if(NOT configuration STREQUAL "")
            string(TOUPPER "${configuration}" configuration_upper)
            string(APPEND pause_environment_config_flags
                "$<$<CONFIG:${configuration}>:${CMAKE_CXX_FLAGS_${configuration_upper}}>")
        endif()
    endforeach()
    get_property(pause_environment_runtime_set TARGET pause_integration_test
        PROPERTY MSVC_RUNTIME_LIBRARY SET)
    if(pause_environment_runtime_set)
        set(pause_environment_runtime
            "$<TARGET_GENEX_EVAL:pause_integration_test,$<TARGET_PROPERTY:pause_integration_test,MSVC_RUNTIME_LIBRARY>>")
    else()
        # CMake's default when no runtime library property was specified.
        set(pause_environment_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    add_test(NAME pause_environment_compile
        COMMAND ${CMAKE_COMMAND} "-DCXX=${CMAKE_CXX_COMPILER}"
            "-DCXX_FLAGS=${CMAKE_CXX_FLAGS} ${pause_environment_config_flags}"
            "-DMSVC_RUNTIME_LIBRARY=${pause_environment_runtime}"
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DSTDEXEC_INCLUDE_DIR=${stdexec_SOURCE_DIR}/include"
            "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/pause-environment-check/$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_pause_environment.cmake")
    set_tests_properties(pause_environment_compile PROPERTIES TIMEOUT 120)
endif()

option(PAUSE_BUILD_DESIGN_TESTS
       "Build design-model and pinned-stdexec contract tests (not a Pause implementation test)"
       OFF)

if(PAUSE_BUILD_DESIGN_TESTS)
    enable_testing()
    include(tests/instrument_stop_token.cmake)

    foreach(test_name IN ITEMS
            pause_state_model_test
            stdexec_stop_callback_test
            stdexec_stop_callback_hook_test
            stdexec_callback_affine_lifetime_test
            stdexec_task_lifetime_test
            stdexec_standard_task_api_test
            stdexec_standard_task_lifetime_test
            stdexec_deep_chain_test
            stdexec_standard_task_scheduler_test)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE pause::pause)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
    endforeach()

    add_test(NAME pause_deep_chain_model_test
             COMMAND pause_state_model_test --chain-depth 16 --chain-only 1)
    set_tests_properties(pause_deep_chain_model_test PROPERTIES TIMEOUT 60)

    target_include_directories(stdexec_stop_callback_hook_test PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/pause-test-hooks"
        "${stdexec_SOURCE_DIR}/include/stdexec")
endif()
