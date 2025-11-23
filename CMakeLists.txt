cmake_minimum_required(VERSION 3.16)

project(hyper_cache VERSION 1.0.0 LANGUAGES CXX)

#include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
#conan_basic_setup(TARGETS)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE Release) 

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

find_package(benchmark REQUIRED)

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fsanitize-recover=address,undefined -fno-stack-protector -fno-omit-frame-pointer -fno-var-tracking)
    add_link_options(-fsanitize=address)
endif()

if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fsanitize-recover=thread -fno-stack-protector -fno-omit-frame-pointer -fno-var-tracking)
    add_link_options(-fsanitize=thread)
endif()

file(GLOB cache_files ${CMAKE_CURRENT_SOURCE_DIR}/src/*/*.cc )

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_library(hyper_cache STATIC ${cache_files})
install(TARGETS hyper_cache DESTINATION lib)

add_executable(cache_rw_mixed_bench ${CMAKE_CURRENT_SOURCE_DIR}/src/cache_rw_mixed_bench.cc)
target_link_libraries(cache_rw_mixed_bench PRIVATE benchmark::benchmark pthread hyper_cache)

add_executable(cache_rw_separate_bench ${CMAKE_CURRENT_SOURCE_DIR}/src/cache_rw_separate_bench.cc)
target_link_libraries(cache_rw_separate_bench PRIVATE benchmark::benchmark pthread hyper_cache)
