cmake_minimum_required(VERSION 3.16)
project(voice_runtime_simulation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(voice_runtime_simulation tests/simulation_main.cpp)
target_include_directories(voice_runtime_simulation PRIVATE include tests)
if(UNIX)
    target_link_libraries(voice_runtime_simulation PRIVATE pthread)
endif()
