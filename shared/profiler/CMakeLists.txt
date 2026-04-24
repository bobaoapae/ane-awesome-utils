cmake_minimum_required(VERSION 3.20)
project(ane_profiler_core C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  add_compile_options(/W4 /permissive-)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Core library — mix of header-only facilities (SpscByteStream, FileFormat)
# and concrete sinks/writers (RawFileSink, CompressedSink, WriterThread,
# CaptureController) as they land.
# miniz: single-file zlib-compatible deflate library (bundled).
add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz)
if(MSVC)
    # miniz has known, intentional C-style signed/unsigned comparisons,
    # unused parameters, etc. Silence them so our /W4 build stays clean.
    target_compile_options(miniz PRIVATE /w)
endif()

add_library(ane_profiler_core STATIC
    src/RawFileSink.cpp
    src/ZlibFileSink.cpp
    src/CaptureController.cpp
    src/DeepProfilerController.cpp
)
target_include_directories(ane_profiler_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ane_profiler_core PUBLIC miniz)

# Tests ---------------------------------------------------------------------
enable_testing()

function(ane_add_test NAME SRC)
    add_executable(${NAME} ${SRC})
    target_link_libraries(${NAME} PRIVATE ane_profiler_core)
    if(NOT MSVC)
        target_link_libraries(${NAME} PRIVATE pthread)
    endif()
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

ane_add_test(test_spsc_byte_stream   tests/test_spsc_byte_stream.cpp)
ane_add_test(test_file_format        tests/test_file_format.cpp)
ane_add_test(test_zlib_file_sink     tests/test_zlib_file_sink.cpp)
ane_add_test(test_capture_controller tests/test_capture_controller.cpp)
ane_add_test(test_aneprof_controller tests/test_aneprof_controller.cpp)

# Sample .flmc generator used by Python-side validation.
add_executable(make_sample_flmc tools/make_sample_flmc.cpp)
target_link_libraries(make_sample_flmc PRIVATE ane_profiler_core)
if(NOT MSVC)
    target_link_libraries(make_sample_flmc PRIVATE pthread)
endif()
