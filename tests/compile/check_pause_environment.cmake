# 测试项目：编译 pause_environment_compile.cpp 的 1 个合法和 4 个非法环境。
# 先验证正例，排除缺依赖等伪失败；负例须正常启动编译器且非零退出。
# 继承当前构建配置/CRT，各次编译限时 90 秒，日志和对象文件写入 OUTPUT_DIRECTORY。
# 由 CTest pause_environment_compile 传入参数；不是运行时压力测试。
# Run with the same compiler environment as the normal CMake build. A valid
# receiver is compiled first, ruling out missing includes/toolchain failures as
# a false positive for either deliberately invalid receiver environment.
foreach(required IN ITEMS CXX CXX_FLAGS MSVC_RUNTIME_LIBRARY SOURCE_DIR
                          STDEXEC_INCLUDE_DIR OUTPUT_DIRECTORY)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing -D${required}=... for environment compile tests")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
separate_arguments(configuration_args NATIVE_COMMAND "${CXX_FLAGS}")
get_filename_component(compiler_name "${CXX}" NAME)
if(compiler_name MATCHES "^(cl|clang-cl)(\\.exe)?$")
    # Honor CMake's selected CRT without duplicating Debug/Release settings.
    if(MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded")
        list(APPEND configuration_args /MT)
    elseif(MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDLL")
        list(APPEND configuration_args /MD)
    elseif(MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDebug")
        list(APPEND configuration_args /MTd)
    elseif(MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDebugDLL")
        list(APPEND configuration_args /MDd)
    elseif(NOT MSVC_RUNTIME_LIBRARY STREQUAL "")
        message(FATAL_ERROR "Unsupported MSVC_RUNTIME_LIBRARY: ${MSVC_RUNTIME_LIBRARY}")
    endif()
    set(common_args ${configuration_args} /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc
        "/I${SOURCE_DIR}" "/I${STDEXEC_INCLUDE_DIR}" /c)
    set(object_arg "/Fo${OUTPUT_DIRECTORY}/pause_environment_compile.obj")
else()
    set(common_args ${configuration_args} -std=c++23 "-I${SOURCE_DIR}" "-I${STDEXEC_INCLUDE_DIR}" -c)
    set(object_arg -o "${OUTPUT_DIRECTORY}/pause_environment_compile.o")
endif()

foreach(independent_mode RANGE 0 1)
foreach(environment_case RANGE 0 4)
    execute_process(
        COMMAND "${CXX}" ${common_args}
            "-DPAUSE_ENVIRONMENT_CASE=${environment_case}"
            "-DPAUSE_ENABLE_INDEPENDENT_BRANCH=${independent_mode}"
            "${SOURCE_DIR}/tests/compile/pause_environment_compile.cpp" ${object_arg}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE compiler_stdout
        ERROR_VARIABLE compiler_stderr
        TIMEOUT 90)
    file(WRITE "${OUTPUT_DIRECTORY}/environment-${environment_case}-independent-${independent_mode}.log"
        "flags=${common_args}\nexit=${result}\n${compiler_stdout}\n${compiler_stderr}")
    if(environment_case EQUAL 0)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Valid environment failed to compile: ${compiler_stdout}${compiler_stderr}")
        endif()
    elseif(result EQUAL 0)
        message(FATAL_ERROR "Invalid environment case ${environment_case} unexpectedly compiled")
    elseif(NOT result MATCHES "^[0-9]+$")
        message(FATAL_ERROR "Compiler failed to execute for negative case ${environment_case}: ${result}")
    endif()
endforeach()
endforeach()
message(STATUS "Pause environment compile checks passed (default and independent-branch API): valid, missing scheduler, wrong/missing stop token, invalid scheduler")
