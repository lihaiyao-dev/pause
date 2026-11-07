include(CMakePackageConfigHelpers)

# Header-only packages do not need an architecture-specific lib directory.
set(pause_cmake_dir "${CMAKE_INSTALL_DATADIR}/cmake/pause")
foreach(destination IN ITEMS "${CMAKE_INSTALL_INCLUDEDIR}" "${CMAKE_INSTALL_DATADIR}")
    if(IS_ABSOLUTE "${destination}" OR destination MATCHES "(^|/)\\.\\.(/|$)|[;\\\\\n\r]")
        message(FATAL_ERROR "Pause packages require relative install directories inside the prefix")
    endif()
endforeach()
file(RELATIVE_PATH pause_config_to_include
    "${CMAKE_CURRENT_BINARY_DIR}/layout/${pause_cmake_dir}"
    "${CMAKE_CURRENT_BINARY_DIR}/layout/${CMAKE_INSTALL_INCLUDEDIR}")
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/pauseConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pauseConfig.cmake"
    INSTALL_DESTINATION "${pause_cmake_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/pauseConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)

install(TARGETS pause EXPORT pauseTargets COMPONENT Pause)
install(EXPORT pauseTargets NAMESPACE pause:: DESTINATION "${pause_cmake_dir}" COMPONENT Pause)
install(FILES "${PROJECT_SOURCE_DIR}/pause.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT Pause)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/pauseConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/pauseConfigVersion.cmake"
              "${CMAKE_CURRENT_LIST_DIR}/PauseDependency.cmake"
              "${PROJECT_SOURCE_DIR}/cmake/check_stdexec.cpp"
    DESTINATION "${pause_cmake_dir}" COMPONENT Pause)
install(FILES "${PROJECT_SOURCE_DIR}/docs/packaging.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pause" COMPONENT Pause)
install(FILES "${PROJECT_SOURCE_DIR}/docs/user-guide.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pause" RENAME README.md COMPONENT Pause)
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/LICENSE")
    message(FATAL_ERROR "Pause distribution requires the project LICENSE file")
endif()
set(pause_release_documents LICENSE CHANGELOG.md THIRD_PARTY_NOTICES.md)
foreach(document IN LISTS pause_release_documents)
    install(FILES "${PROJECT_SOURCE_DIR}/${document}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/pause" COMPONENT Pause)
endforeach()
install(FILES "${PROJECT_SOURCE_DIR}/examples/cli/simple_workflow.cpp"
              "${PROJECT_SOURCE_DIR}/examples/common/example_support.hpp"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pause/examples" COMPONENT Pause)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/InstalledExample.cmake.in"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pause/examples" RENAME CMakeLists.txt COMPONENT Pause)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/manage-pause.cmake" DESTINATION . COMPONENT Pause)

# Explicit inventory: never glob a staging prefix that may contain other libraries.
set(pause_package_files
    "${CMAKE_INSTALL_INCLUDEDIR}/pause.hpp"
    "${pause_cmake_dir}/pauseTargets.cmake"
    "${pause_cmake_dir}/pauseConfig.cmake"
    "${pause_cmake_dir}/pauseConfigVersion.cmake"
    "${pause_cmake_dir}/PauseDependency.cmake"
    "${pause_cmake_dir}/check_stdexec.cpp"
    "${CMAKE_INSTALL_DATADIR}/pause/packaging.md"
    "${CMAKE_INSTALL_DATADIR}/pause/README.md"
    "${CMAKE_INSTALL_DATADIR}/pause/examples/simple_workflow.cpp"
    "${CMAKE_INSTALL_DATADIR}/pause/examples/example_support.hpp"
    "${CMAKE_INSTALL_DATADIR}/pause/examples/CMakeLists.txt"
    "manage-pause.cmake")
foreach(document IN LISTS pause_release_documents)
    list(APPEND pause_package_files "${CMAKE_INSTALL_DATADIR}/pause/${document}")
endforeach()
configure_file("${CMAKE_CURRENT_LIST_DIR}/PackageManifest.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PackageManifest.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/PackageManifest.cmake" COMPONENT Pause)

if(PROJECT_IS_TOP_LEVEL)
    set(CPACK_PACKAGE_NAME pause)
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C++23 cooperative pause header-only library")
    set(CPACK_PACKAGE_VENDOR "Pause contributors")
    set(CPACK_PACKAGE_DESCRIPTION_FILE "${PROJECT_SOURCE_DIR}/README.md")
    set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
    set(CPACK_GENERATOR "ZIP;TGZ")
    set(CPACK_PACKAGE_FILE_NAME "pause-${PROJECT_VERSION}")
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    # Package only Pause, even if the caller enabled installation of a dependency.
    set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};pause;Pause;/")
    include(CPack)
endif()
