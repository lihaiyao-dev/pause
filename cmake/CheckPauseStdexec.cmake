# stdexec's project version does not identify the required interfaces. In
# particular, member affine() customization reached main in PR #2103 on
# 2026-07-05 (5e22875aad04fd933a9f0405e09fe85c77bf07db).
# Check the selected source, including FetchContent source overrides/archives.
function(pause_check_stdexec)
    # stdexec finds Threads in its own directory; make the imported dependency
    # visible here too so try_compile can export it with the probe target.
    find_package(Threads REQUIRED)
    # try_compile can import imported targets, but not a normal build target
    # such as STDEXEC::stdexec. Preserve its actual usage requirements instead
    # of maintaining a second compiler-specific flag list.
    add_library(pause_stdexec_requirements INTERFACE IMPORTED)
    foreach(property IN ITEMS
            INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
            INTERFACE_COMPILE_FEATURES INTERFACE_COMPILE_OPTIONS
            INTERFACE_COMPILE_DEFINITIONS INTERFACE_LINK_LIBRARIES)
        get_target_property(requirement STDEXEC::stdexec ${property})
        if(NOT requirement STREQUAL "requirement-NOTFOUND")
            set_property(TARGET pause_stdexec_requirements PROPERTY ${property} "${requirement}")
        endif()
    endforeach()
    if(MSVC)
        target_compile_options(pause_stdexec_requirements INTERFACE /EHsc)
    endif()

    # Compile only: usable with cross toolchains and no target-side execution.
    # NO_CACHE deliberately rechecks after a ref/source/toolchain change.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    message(STATUS "Checking stdexec interfaces required by Pause")
    try_compile(pause_stdexec_compatible
        SOURCES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tests/check_stdexec.cpp"
        NO_CACHE
        LINK_LIBRARIES pause_stdexec_requirements
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        OUTPUT_VARIABLE compiler_output)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pause-stdexec-check.log" "${compiler_output}")
    if(NOT pause_stdexec_compatible)
        message(FATAL_ERROR
            "The selected stdexec source or C++23 toolchain does not satisfy Pause's requirements.\n"
            "Required: sender member .affine() customization (stdexec PR #2103), "
            "completion_behavior, and the task/Pause sender interfaces.\n"
            "Known minimum upstream main revision: 5e22875aad04fd933a9f0405e09fe85c77bf07db "
            "(2026-07-05). A project version such as 0.11.0 is not sufficient.\n"
            "Select a compatible PAUSE_STDEXEC_GIT_TAG or FETCHCONTENT_SOURCE_DIR_STDEXEC.\n"
            "Compiler details: ${CMAKE_CURRENT_BINARY_DIR}/pause-stdexec-check.log")
    endif()
    message(STATUS "Checking stdexec interfaces required by Pause - passed")
endfunction()
