# Shared by the source project and the relocatable installed package.
include_guard(GLOBAL)

function(pause_resolve_stdexec)
    option(PAUSE_FETCH_STDEXEC "Download stdexec when no installed package is found" ON)
    if(NOT TARGET STDEXEC::stdexec)
        # Do not request stdexec's numeric version: 0.11.0 identifies both
        # compatible and incompatible revisions. Check the actual API below.
        find_package(stdexec CONFIG QUIET)
        if(NOT TARGET STDEXEC::stdexec)
            if(stdexec_FOUND OR stdexec_CONSIDERED_CONFIGS)
                message(FATAL_ERROR
                    "An installed stdexec package was found but does not provide STDEXEC::stdexec. "
                    "Fix stdexec_DIR/CMAKE_PREFIX_PATH; Pause will not replace an existing package.")
            endif()
            if(NOT PAUSE_FETCH_STDEXEC OR CMAKE_DISABLE_FIND_PACKAGE_stdexec)
                message(FATAL_ERROR
                    "stdexec was not found and downloading is disabled. Install a compatible "
                    "stdexec package and set stdexec_DIR or CMAKE_PREFIX_PATH.")
            endif()
            include(FetchContent)
            set(PAUSE_STDEXEC_GIT_TAG "a23aa7abc8796edf158ff67afaf732a6dad3e2a5"
                CACHE STRING "stdexec revision (must satisfy Pause's configure-time compatibility check)")
            # Normal variables keep dependency defaults local to this function.
            set(STDEXEC_BUILD_EXAMPLES OFF)
            set(STDEXEC_BUILD_TESTS OFF)
            set(STDEXEC_INSTALL OFF)
            message(STATUS "stdexec not found; fetching ${PAUSE_STDEXEC_GIT_TAG}")
            FetchContent_Declare(stdexec
                GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
                GIT_TAG "${PAUSE_STDEXEC_GIT_TAG}"
                GIT_PROGRESS TRUE)
            FetchContent_MakeAvailable(stdexec)
        endif()
    endif()
    if(NOT TARGET STDEXEC::stdexec)
        message(FATAL_ERROR "The selected stdexec dependency does not provide STDEXEC::stdexec")
    endif()

    # Only the project's instrumented tests need a physical header directory.
    # No assumption that an installed package has an upstream source tree.
    get_target_property(include_dirs STDEXEC::stdexec INTERFACE_INCLUDE_DIRECTORIES)
    foreach(include_dir IN LISTS include_dirs)
        string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1" include_dir "${include_dir}")
        if(EXISTS "${include_dir}/stdexec/execution.hpp")
            set(pause_stdexec_include_dir "${include_dir}" PARENT_SCOPE)
            break()
        endif()
    endforeach()
endfunction()

# stdexec's project version does not identify the required interfaces. In
# particular, member affine() customization reached main in PR #2103 on
# 2026-07-05 (5e22875aad04fd933a9f0405e09fe85c77bf07db).
# Check the selected source, including FetchContent source overrides/archives.
function(pause_check_stdexec)
    # The old implementation lets its stop callback arrive at the child
    # barrier after the last child has already completed. This can complete
    # twice or deadlock. API-only probes cannot detect that runtime defect.
    set(when_all_header "${pause_stdexec_include_dir}/stdexec/__detail/__when_all.hpp")
    if(EXISTS "${when_all_header}")
        file(READ "${when_all_header}" when_all_source)
        string(FIND "${when_all_source}" "__state_->__count_.fetch_add" unsafe_stop_callback)
        if(NOT unsafe_stop_callback EQUAL -1)
            message(FATAL_ERROR
                "The selected stdexec has a known when_all cancellation race; automatic replacement is disabled.\n"
                "Use a23aa7abc8796edf158ff67afaf732a6dad3e2a5 or a revision containing its fix.\n"
                "Update stdexec_DIR/CMAKE_PREFIX_PATH, PAUSE_STDEXEC_GIT_TAG, or FETCHCONTENT_SOURCE_DIR_STDEXEC.")
        endif()
    endif()
    if(ARGC EQUAL 2)
        set(probe_source "${ARGV0}")
        set(pause_include_dir "${ARGV1}")
    else()
        set(probe_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_stdexec.cpp")
        set(pause_include_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    endif()
    # stdexec finds Threads in its own directory; make the imported dependency
    # visible here too so try_compile can export it with the probe target.
    find_package(Threads REQUIRED)
    # try_compile can import imported targets, but not a normal build target
    # such as STDEXEC::stdexec. Preserve its actual usage requirements instead
    # of maintaining a second compiler-specific flag list.
    if(NOT TARGET pause_stdexec_requirements)
        add_library(pause_stdexec_requirements INTERFACE IMPORTED)
    endif()
    foreach(property IN ITEMS
            INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
            INTERFACE_COMPILE_FEATURES INTERFACE_COMPILE_OPTIONS
            INTERFACE_COMPILE_DEFINITIONS INTERFACE_LINK_LIBRARIES)
        get_target_property(requirement STDEXEC::stdexec ${property})
        if(NOT requirement STREQUAL "requirement-NOTFOUND")
            set_property(TARGET pause_stdexec_requirements PROPERTY ${property} "${requirement}")
        endif()
    endforeach()
    target_include_directories(pause_stdexec_requirements INTERFACE "${pause_include_dir}")
    if(MSVC)
        target_compile_options(pause_stdexec_requirements INTERFACE /EHsc)
    endif()

    # Compile only: usable with cross toolchains and no target-side execution.
    # NO_CACHE deliberately rechecks after a ref/source/toolchain change.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    message(STATUS "Checking stdexec interfaces required by Pause")
    try_compile(pause_stdexec_compatible
        SOURCES "${probe_source}"
        NO_CACHE
        LINK_LIBRARIES pause_stdexec_requirements
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        OUTPUT_VARIABLE compiler_output)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pause-stdexec-check.log" "${compiler_output}")
    if(NOT pause_stdexec_compatible)
        message(WARNING "The selected stdexec/toolchain is incompatible; automatic replacement is disabled.")
        message(FATAL_ERROR
            "The selected stdexec source or C++23 toolchain does not satisfy Pause's requirements.\n"
            "Required: sender member .affine() customization (stdexec PR #2103), "
            "completion_behavior, task/Pause senders, and native thread-pool schedulers.\n"
            "Reviewed baseline: a23aa7abc8796edf158ff67afaf732a6dad3e2a5. "
            "A project version such as 0.11.0 is not sufficient.\n"
            "For an installed dependency, fix stdexec_DIR/CMAKE_PREFIX_PATH.\n"
            "For a fetched dependency, select a compatible PAUSE_STDEXEC_GIT_TAG "
            "or FETCHCONTENT_SOURCE_DIR_STDEXEC.\n"
            "Compiler details: ${CMAKE_CURRENT_BINARY_DIR}/pause-stdexec-check.log")
    endif()
    message(STATUS "Checking stdexec interfaces required by Pause - passed")
endfunction()
