cmake_minimum_required(VERSION 3.27)
project(spsc_queue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

add_library(spsc_queue INTERFACE)
target_include_directories(spsc_queue INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(spsc_queue INTERFACE Threads::Threads)

set(SANITIZER "none" CACHE STRING "Sanitizer to enable")
set_property(CACHE SANITIZER PROPERTY STRINGS none thread address)

function(configure_target t)
    target_compile_options(${t} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion>
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->)
    if(SANITIZER STREQUAL "thread")
        target_compile_options(${t} PRIVATE -fsanitize=thread -g -fno-omit-frame-pointer)
        target_link_options(${t} PRIVATE -fsanitize=thread)
    elseif(SANITIZER STREQUAL "address")
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -g -fno-omit-frame-pointer)
        target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_executable(spsc_queue_main main.cpp)
target_link_libraries(spsc_queue_main PRIVATE spsc_queue)
configure_target(spsc_queue_main)

enable_testing()
add_test(NAME spsc_queue_main COMMAND spsc_queue_main)