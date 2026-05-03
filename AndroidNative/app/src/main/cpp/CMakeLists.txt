
# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 4.0.2)

# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("emulatordetector")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# shared/profiler — cross-platform profiler core (CaptureController,
# DeepProfilerController, sinks, miniz). Path resolves up from the Android
# Gradle module to the repo's shared dir.
set(SHARED_PROFILER ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../shared/profiler)

# Compile miniz (zlib-compatible deflate, single C file) directly into our
# library — avoids a separate static lib step and keeps the .so self-contained.
set_source_files_properties(${SHARED_PROFILER}/third_party/miniz/miniz.c PROPERTIES
    COMPILE_FLAGS "-w")  # silence miniz's intentional sign-comparison warnings

add_library(${CMAKE_PROJECT_NAME} SHARED
        # ANE existing sources
        Main.cpp
        EmulatorDetection.cpp
        CrashSignalHandler.cpp
        AirIMEGuard.cpp
        probe_native.cpp
        alloc_tracer.cpp
        defer_drain.cpp

        # Profiler core (shared with Windows / iOS / macOS)
        ${SHARED_PROFILER}/src/RawFileSink.cpp
        ${SHARED_PROFILER}/src/ZlibFileSink.cpp
        ${SHARED_PROFILER}/src/CaptureController.cpp
        ${SHARED_PROFILER}/src/DeepProfilerController.cpp
        ${SHARED_PROFILER}/third_party/miniz/miniz.c

        # Android-specific runtime hook
        profiler/AndroidRuntimeHook.cpp

        # Android-specific render hook (Phase 6)
        profiler/AndroidRenderHook.cpp

        # Android-specific deep memory hook (Phase 5 — GCHeap::Alloc shadowhook)
        profiler/AndroidDeepMemoryHook.cpp

        # JNI bridge for AS3 → native profiler
        profiler/AndroidProfilerBridge.cpp)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${SHARED_PROFILER}/include
        ${SHARED_PROFILER}/third_party/miniz
        ${CMAKE_CURRENT_SOURCE_DIR}/profiler)

# shadowhook — prebuilt shared library for inline hooking, distributed via prefab AAR.
find_package(shadowhook REQUIRED CONFIG)

# Specifies libraries CMake should link to your target library. You
# can link libraries from various origins, such as libraries defined in this
# build script, prebuilt third-party libraries, or Android system libraries.
target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        android
        log
        shadowhook::shadowhook)
