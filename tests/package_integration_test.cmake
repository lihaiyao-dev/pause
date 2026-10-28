# Run in a compiler-enabled shell after configuring the Pause packaging build
# and independently installing stdexec. No global install prefix is modified.
cmake_minimum_required(VERSION 3.25)
foreach(required IN ITEMS PAUSE_BINARY_DIR STDEXEC_PREFIX OUTPUT_DIRECTORY)
    if(NOT IS_ABSOLUTE "${${required}}")
        message(FATAL_ERROR "Set -D${required}=<absolute-path>")
    endif()
endforeach()
if(EXISTS "${OUTPUT_DIRECTORY}")
    message(FATAL_ERROR "Choose a fresh OUTPUT_DIRECTORY")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}/extract")
get_filename_component(source_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(consumer "${source_root}/tests/package_consumer")
set(prefix "${OUTPUT_DIRECTORY}/prefix with spaces")
set(stdexec_dir "${STDEXEC_PREFIX}/lib/cmake/stdexec")
find_program(cpack NAMES cpack REQUIRED)
find_program(ctest NAMES ctest REQUIRED)
find_program(ninja NAMES ninja HINTS "${NINJA_DIRECTORY}" REQUIRED)
set(generator_args -G Ninja "-DCMAKE_MAKE_PROGRAM=${ninja}" -DCMAKE_BUILD_TYPE=Release)

function(run expected pattern)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
    file(APPEND "${OUTPUT_DIRECTORY}/integration.log" "${ARGN}\n${out}${err}\n")
    string(REGEX REPLACE "[ \t\r\n]+" " " diagnostic "${out}${err}")
    if((expected STREQUAL "pass" AND NOT result STREQUAL "0")
       OR (expected STREQUAL "fail" AND result STREQUAL "0")
       OR NOT diagnostic MATCHES "${pattern}")
        message(FATAL_ERROR "Unexpected result ${result}: ${out}${err}")
    endif()
endfunction()

run(pass "generated" "${cpack}" --config "${PAUSE_BINARY_DIR}/CPackConfig.cmake"
    -B "${OUTPUT_DIRECTORY}/archives")
file(ARCHIVE_EXTRACT INPUT "${OUTPUT_DIRECTORY}/archives/pause-0.1.0.zip"
    DESTINATION "${OUTPUT_DIRECTORY}/extract")
set(package "${OUTPUT_DIRECTORY}/extract/pause-0.1.0")
file(READ "${package}/share/cmake/pause/pauseTargets.cmake" exported_targets)
string(FIND "${exported_targets}" "${source_root}" source_reference)
string(FIND "${exported_targets}" "${PAUSE_BINARY_DIR}" build_reference)
if(NOT source_reference EQUAL -1 OR NOT build_reference EQUAL -1
   OR exported_targets MATCHES "/EHsc")
    message(FATAL_ERROR "The exported target leaked producer paths or compiler options")
endif()
run(pass "completed" "${CMAKE_COMMAND}" -DACTION=install "-DPREFIX=${prefix}"
    -P "${package}/manage-pause.cmake")
set(document_files share/pause/README.md share/pause/packaging.md
    share/pause/examples/CMakeLists.txt share/pause/examples/simple_workflow.cpp
    share/pause/examples/example_support.hpp)
foreach(relative IN LISTS document_files)
    file(SHA256 "${package}/${relative}" packaged_hash)
    file(SHA256 "${prefix}/${relative}" installed_hash)
    if(NOT packaged_hash STREQUAL installed_hash)
        message(FATAL_ERROR "Installed documentation differs from the package: ${relative}")
    endif()
endforeach()
file(WRITE "${prefix}/unrelated.txt" "keep")
run(pass "Build files" "${CMAKE_COMMAND}" -S "${consumer}" -B "${OUTPUT_DIRECTORY}/consumer"
    ${generator_args} "-DCMAKE_PREFIX_PATH=${prefix}" "-Dstdexec_DIR=${stdexec_dir}"
    -DPAUSE_FETCH_STDEXEC=OFF)
run(pass "" "${CMAKE_COMMAND}" --build "${OUTPUT_DIRECTORY}/consumer")
run(pass "100% tests passed" "${ctest}" --test-dir "${OUTPUT_DIRECTORY}/consumer" --output-on-failure)

# Move the entire installed prefix, then discover/build from its new location.
set(moved "${OUTPUT_DIRECTORY}/relocated")
file(RENAME "${prefix}" "${moved}")
set(prefix "${moved}")
run(pass "Build files" "${CMAKE_COMMAND}" -S "${consumer}" -B "${OUTPUT_DIRECTORY}/relocated-consumer"
    ${generator_args} "-DCMAKE_PREFIX_PATH=${prefix}" "-Dstdexec_DIR=${stdexec_dir}"
    -DPAUSE_FETCH_STDEXEC=OFF)
run(pass "" "${CMAKE_COMMAND}" --build "${OUTPUT_DIRECTORY}/relocated-consumer")
run(pass "100% tests passed" "${ctest}" --test-dir "${OUTPUT_DIRECTORY}/relocated-consumer" --output-on-failure)

# The documented example must build using only the relocated installed package.
run(pass "Build files" "${CMAKE_COMMAND}"
    -S "${prefix}/share/pause/examples" -B "${OUTPUT_DIRECTORY}/installed-example"
    ${generator_args} "-DCMAKE_PREFIX_PATH=${prefix}" "-Dstdexec_DIR=${stdexec_dir}"
    -DPAUSE_FETCH_STDEXEC=OFF)
run(pass "" "${CMAKE_COMMAND}" --build "${OUTPUT_DIRECTORY}/installed-example")
run(pass "100% tests passed" "${ctest}" --test-dir "${OUTPUT_DIRECTORY}/installed-example" --output-on-failure)

# A discovered but incompatible package must fail even with fetching enabled.
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}/bad-dependency/include")
file(WRITE "${OUTPUT_DIRECTORY}/bad-dependency/stdexec-config.cmake"
    "add_library(STDEXEC::stdexec INTERFACE IMPORTED)\nset_target_properties(STDEXEC::stdexec PROPERTIES INTERFACE_INCLUDE_DIRECTORIES \"\${CMAKE_CURRENT_LIST_DIR}/include\")\n")
run(fail "automatic replacement is disabled" "${CMAKE_COMMAND}"
    -S "${consumer}" -B "${OUTPUT_DIRECTORY}/incompatible" ${generator_args}
    "-DCMAKE_PREFIX_PATH=${prefix}" "-Dstdexec_DIR=${OUTPUT_DIRECTORY}/bad-dependency"
    -DPAUSE_FETCH_STDEXEC=ON)
if(EXISTS "${OUTPUT_DIRECTORY}/incompatible/_deps")
    message(FATAL_ERROR "An incompatible installation incorrectly triggered a download")
endif()
run(fail "downloading is disabled" "${CMAKE_COMMAND}"
    -S "${consumer}" -B "${OUTPUT_DIRECTORY}/missing" ${generator_args}
    "-DCMAKE_PREFIX_PATH=${prefix}" -DCMAKE_DISABLE_FIND_PACKAGE_stdexec=ON)
run(fail "compatible with requested version" "${CMAKE_COMMAND}"
    -S "${consumer}" -B "${OUTPUT_DIRECTORY}/wrong-version" ${generator_args}
    "-DCMAKE_PREFIX_PATH=${prefix}" "-Dstdexec_DIR=${stdexec_dir}" -DPAUSE_TEST_VERSION=0.2)

# Verify both archive formats carry usable, identical payloads.
file(ARCHIVE_EXTRACT INPUT "${OUTPUT_DIRECTORY}/archives/pause-0.1.0.tar.gz"
    DESTINATION "${OUTPUT_DIRECTORY}/tgz")
file(SHA256 "${package}/pause-manifest.txt" zip_hash)
file(SHA256 "${OUTPUT_DIRECTORY}/tgz/pause-0.1.0/pause-manifest.txt" tgz_hash)
if(NOT zip_hash STREQUAL tgz_hash)
    message(FATAL_ERROR "Archive payloads differ")
endif()
run(pass "completed" "${CMAKE_COMMAND}" -DACTION=update "-DPREFIX=${prefix}"
    -P "${OUTPUT_DIRECTORY}/tgz/pause-0.1.0/manage-pause.cmake")
foreach(relative IN LISTS document_files)
    if(NOT EXISTS "${prefix}/${relative}")
        message(FATAL_ERROR "Update lost installed documentation: ${relative}")
    endif()
endforeach()
run(pass "completed" "${CMAKE_COMMAND}" -DACTION=uninstall "-DPREFIX=${prefix}"
    -P "${prefix}/manage-pause.cmake")
if(NOT EXISTS "${prefix}/unrelated.txt" OR EXISTS "${prefix}/include/pause.hpp")
    message(FATAL_ERROR "Uninstall violated ownership")
endif()
foreach(relative IN LISTS document_files)
    if(EXISTS "${prefix}/${relative}")
        message(FATAL_ERROR "Uninstall left managed documentation: ${relative}")
    endif()
endforeach()
run(fail "Could not find a package configuration file provided by" "${CMAKE_COMMAND}" -S "${consumer}" -B "${OUTPUT_DIRECTORY}/uninstalled"
    ${generator_args} "-DCMAKE_PREFIX_PATH=${prefix}" "-Dstdexec_DIR=${stdexec_dir}")
message(STATUS "Pause package integration tests passed; log: ${OUTPUT_DIRECTORY}/integration.log")
