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
            set(PAUSE_STDEXEC_GIT_TAG "f0e8ae6fdc6c188389b146bb854b80d399724b04"
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
