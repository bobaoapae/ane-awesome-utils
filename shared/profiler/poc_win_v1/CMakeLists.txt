cmake_minimum_required(VERSION 3.20)
project(ane_profiler_poc_v1 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  add_compile_options(/W4 /permissive-)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Hook DLL — injected into the AIR process, patches the IAT slot of
# ws2_32!send in Adobe AIR.dll so every outgoing byte the runtime pushes
# through its TCP socket (Scout wire) also lands in our dump.
add_library(air_profiler_probe SHARED profiler_poc_hook.cpp)
set_target_properties(air_profiler_probe PROPERTIES OUTPUT_NAME "air_profiler_probe")
target_link_libraries(air_profiler_probe PRIVATE ws2_32 psapi)

# Injector EXE — spawns target CREATE_SUSPENDED, LoadLibrary via
# CreateRemoteThread, resumes.
add_executable(profiler_inject profiler_poc_inject.cpp)
target_compile_definitions(profiler_inject PRIVATE _UNICODE UNICODE)
set_target_properties(profiler_inject PROPERTIES OUTPUT_NAME "profiler_inject")

install(TARGETS air_profiler_probe profiler_inject
        RUNTIME DESTINATION .
        LIBRARY DESTINATION .)
