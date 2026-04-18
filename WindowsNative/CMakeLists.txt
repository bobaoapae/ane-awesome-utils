cmake_minimum_required(VERSION 3.29)
project(AneAwesomeUtilsWindows)

set(CMAKE_CXX_STANDARD 20)
add_definitions(-D_WIN32_WINNT=0x0601)

# Definindo as opções de compilação para Debug e Release
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /D_ITERATOR_DEBUG_LEVEL=2 /MTd")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /D_ITERATOR_DEBUG_LEVEL=0 /MT")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(STATUS "Compiling for x64")
    set(LIBRARY_PATH "C:/AIRSDKs/AIRSDK_51.1.3.10/lib/win64")
    set(INCLUDE_PATH "C:/AIRSDKs/AIRSDK_51.1.3.10/include")
else()
    message(STATUS "Compiling for x86")
    set(LIBRARY_PATH "C:/AIRSDKs/AIRSDK_51.1.3.10/lib/win")
    set(INCLUDE_PATH "C:/AIRSDKs/AIRSDK_51.1.3.10/include")
endif()

if(VCPKG_TARGET_TRIPLET MATCHES "-static$")
    message(STATUS "Compiling static")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    message(STATUS "Compiling dynamic")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")
endif()

link_directories(${LIBRARY_PATH})
include_directories(${INCLUDE_PATH})
include_directories(${CMAKE_SOURCE_DIR}/src)

# Cross-platform profiler core (header-only + CaptureController + sinks).
# Lives under shared/profiler/ at the repo root so Android/Mac/iOS can reuse
# it with their own IRuntimeHook implementations.
set(PROFILER_SHARED_DIR ${CMAKE_SOURCE_DIR}/../shared/profiler)
include_directories(${PROFILER_SHARED_DIR}/include)
include_directories(${PROFILER_SHARED_DIR}/third_party/miniz)

# Compile miniz as a separate static lib so we can keep its warnings silenced
# without polluting our own /W4 build.
add_library(ane_miniz STATIC ${PROFILER_SHARED_DIR}/third_party/miniz/miniz.c)
target_include_directories(ane_miniz PUBLIC ${PROFILER_SHARED_DIR}/third_party/miniz)
set_target_properties(ane_miniz PROPERTIES COMPILE_FLAGS "/w")

add_library(AneAwesomeUtilsWindows SHARED
        src/log.h
        src/log.cpp
        src/AneAwesomeUtilsCsharp.cpp
        src/AneAwesomeUtilsCsharp.h
        src/AneAwesomeUtilsSupport.cpp
        src/DataArray.h
        src/WindowsFilterInputs.h
        src/WindowsFilterInputs.cpp
        src/AudioSafetyHook.h
        src/AudioSafetyHook.cpp
        src/AudioSafetyHookSEH.c
        src/SamplerSafetyHook.h
        src/SamplerSafetyHook.cpp
        # ---- profiler subsystem ----
        ${PROFILER_SHARED_DIR}/src/RawFileSink.cpp
        ${PROFILER_SHARED_DIR}/src/ZlibFileSink.cpp
        ${PROFILER_SHARED_DIR}/src/CaptureController.cpp
        src/profiler/WindowsRuntimeHook.hpp
        src/profiler/WindowsRuntimeHook.cpp
        src/profiler/WindowsAirRuntime.hpp
        src/profiler/WindowsAirRuntime.cpp
        src/profiler/WindowsLoopbackListener.hpp
        src/profiler/WindowsLoopbackListener.cpp
        src/profiler/WindowsDiskMonitor.cpp
        src/profiler/ProfilerAneBindings.hpp
        src/profiler/ProfilerAneBindings.cpp
)

target_link_libraries(AneAwesomeUtilsWindows PRIVATE
        ${LIBRARY_PATH}/FlashRuntimeExtensions.lib
        version.lib
        ws2_32.lib
        ane_miniz)

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /GL")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF /DEBUG /PDBALTPATH:%_PDB%")