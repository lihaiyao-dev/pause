cmake_minimum_required(VERSION 3.25)

project(pause VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(cmake/PauseDependency.cmake)
pause_resolve_stdexec()

include(cmake/CheckPauseStdexec.cmake)
pause_check_stdexec()

add_library(pause INTERFACE)
add_library(pause::pause ALIAS pause)

target_compile_features(pause INTERFACE cxx_std_23)
target_include_directories(
    pause
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
# The installed config links the independently resolved dependency. Exporting
# the fetched build target would incorrectly require exporting stdexec with us.
target_link_libraries(pause INTERFACE $<BUILD_INTERFACE:STDEXEC::stdexec>)
if(MSVC)
    # Resource rollback in the public header requires C++ stack unwinding.
    target_compile_options(pause INTERFACE $<BUILD_INTERFACE:/EHsc>)
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
option(PAUSE_BUILD_EXAMPLES "Build runnable Pause application examples" OFF)
option(PAUSE_BUILD_QT_EXAMPLE "Build the interactive Qt workflow example (requires Qt 6 Widgets)" OFF)

if(PAUSE_BUILD_TESTS)
    enable_testing()
    add_test(NAME pause_package_manager
        COMMAND ${CMAKE_COMMAND}
            "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/package-manager-tests"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_manager_test.cmake")
    set_tests_properties(pause_package_manager PROPERTIES TIMEOUT 60)
    include(tests/instrument_pause_stdexec.cmake)
    foreach(test_name IN ITEMS pause_integration_test pause_sender_test
                               pause_sender_hook_test pause_sender_death_test pause_resource_test
                               pause_task_lifetime_test pause_extreme_test pause_batch_extreme_test
                               pause_transition_extreme_test pause_concurrency_stress_test
                               pause_exception_matrix_test pause_notification_matrix_test
                               pause_contract_death_test pause_outer_lifetime_test pause_host_failure_test)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE pause::pause)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
    endforeach()
    target_compile_definitions(pause_contract_death_test PRIVATE NDEBUG)
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
            "-DSTDEXEC_INCLUDE_DIR=${pause_stdexec_include_dir}"
            "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/pause-environment-check/$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_pause_environment.cmake")
    set_tests_properties(pause_environment_compile PROPERTIES TIMEOUT 120)
endif()

if(PAUSE_BUILD_EXAMPLES)
    add_executable(pause_simple_workflow examples/simple_workflow.cpp)
    add_executable(pause_workflows examples/pause_workflows.cpp)
    add_executable(pause_independent_jobs examples/independent_jobs.cpp)
    add_executable(pause_controlled_workflow examples/controlled_workflow.cpp)
    foreach(example IN ITEMS pause_simple_workflow pause_workflows pause_independent_jobs pause_controlled_workflow)
        target_link_libraries(${example} PRIVATE pause::pause)
        if(MSVC)
            target_compile_options(${example} PRIVATE /utf-8)
        endif()
    endforeach()
    if(PAUSE_BUILD_TESTS)
        add_test(NAME pause_example_simple COMMAND pause_simple_workflow)
        set_tests_properties(pause_example_simple PROPERTIES TIMEOUT 30)
        foreach(scenario IN ITEMS step continue skip-optional cancel work-error control-error)
            add_test(NAME pause_example_controlled_${scenario} COMMAND pause_controlled_workflow ${scenario})
            set_tests_properties(pause_example_controlled_${scenario} PROPERTIES TIMEOUT 30)
        endforeach()
        foreach(scenario IN ITEMS pipeline single-step nested sender-pipeline bulk parallel race
                                  cancel prestop error stopped
                                  optional optional-value optional-error)
            add_test(NAME pause_example_${scenario} COMMAND pause_workflows ${scenario})
            set_tests_properties(pause_example_${scenario} PROPERTIES TIMEOUT 30)
        endforeach()
        add_test(NAME pause_example_independent COMMAND pause_independent_jobs)
        add_test(NAME pause_example_dynamic2 COMMAND pause_independent_jobs --dynamic 2)
        add_test(NAME pause_example_dynamic3 COMMAND pause_independent_jobs --dynamic 3)
        add_test(NAME pause_example_dynamic8 COMMAND pause_independent_jobs --dynamic 8)
        add_test(NAME pause_example_dynamic32 COMMAND pause_independent_jobs --dynamic 32)
        add_test(NAME pause_example_cancel_all COMMAND pause_independent_jobs --cancel-all)
        add_test(NAME pause_example_controller_error COMMAND pause_independent_jobs --controller-error)
        set_tests_properties(pause_example_independent pause_example_dynamic2 pause_example_dynamic3
            pause_example_dynamic8 pause_example_dynamic32 pause_example_cancel_all
            pause_example_controller_error PROPERTIES TIMEOUT 30)
    endif()
endif()

if(PAUSE_BUILD_QT_EXAMPLE)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
    add_executable(pause_qt_workflow examples/qt_workflow.cpp)
    target_link_libraries(pause_qt_workflow PRIVATE pause::pause Qt6::Widgets)
    # Keep Qt's signals/slots/emit macros out of stdexec headers.
    target_compile_definitions(pause_qt_workflow PRIVATE QT_NO_KEYWORDS)
    if(MSVC)
        target_compile_options(pause_qt_workflow PRIVATE /utf-8)
    endif()
    if(PAUSE_BUILD_TESTS)
        foreach(scenario IN ITEMS controls single-step close-paused close-running
                                  cancel-all cancel-running restart-active restart-paused
                                  sender-values sender-error sender-stopped sender-async custom-workflow prestop empty-group)
            add_test(NAME pause_example_qt_${scenario}
                COMMAND pause_qt_workflow --smoke-test ${scenario})
            set_tests_properties(pause_example_qt_${scenario} PROPERTIES
                TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
            if(TARGET Qt6::QOffscreenIntegrationPlugin)
                # windeployqt normally copies only the desktop platform plugin.
                # Tests can use the offscreen plugin from the selected Qt kit.
                set_property(TEST pause_example_qt_${scenario} APPEND PROPERTY ENVIRONMENT
                    "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>")
            endif()
        endforeach()
    endif()
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
        "${pause_stdexec_include_dir}/stdexec")
endif()

option(PAUSE_INSTALL "Generate Pause installation and package rules" ${PROJECT_IS_TOP_LEVEL})
if(PAUSE_INSTALL)
    include(cmake/PausePackaging.cmake)
endif()
