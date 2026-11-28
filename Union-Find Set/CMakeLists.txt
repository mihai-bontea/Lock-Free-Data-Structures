cmake_minimum_required(VERSION 3.27)
project(Union_Find_Set CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimized build if none was specified
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

add_executable(Union_Find_Set main.cpp ConcurrentDsu.h)
target_link_libraries(Union_Find_Set PRIVATE Threads::Threads)

# Optional: tune for the machine you're building on
option(UFS_NATIVE "Build with -march=native" OFF)
if (UFS_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(Union_Find_Set PRIVATE -march=native)
endif()

# ThreadSanitizer build for checking races (slow, use separately)
option(UFS_TSAN "Build with ThreadSanitizer" OFF)
if (UFS_TSAN)
    target_compile_options(Union_Find_Set PRIVATE -fsanitize=thread -g -O1)
    target_link_options(Union_Find_Set PRIVATE -fsanitize=thread)
endif()