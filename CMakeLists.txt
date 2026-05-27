cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

project(sys_prog LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip)
FetchContent_MakeAvailable(googletest)

find_package(Boost COMPONENTS system container REQUIRED) # Ставить через vcpkg

add_subdirectory(allocator)
add_subdirectory(associative_container)
add_subdirectory(common)
