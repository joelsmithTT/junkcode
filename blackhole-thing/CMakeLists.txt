cmake_minimum_required(VERSION 3.10)
project(blackhole-thing)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(Threads REQUIRED)

add_subdirectory(third_party/fmt)
add_subdirectory(src)
include_directories(src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_executable(iommu_test iommu_test.cpp)
target_link_libraries(iommu_test blackhole_thing)

add_executable(console console.cpp)
target_link_libraries(console blackhole_thing)

add_executable(x280_bench x280_bench.cpp)
target_link_libraries(x280_bench blackhole_thing)

add_executable(prefetcher prefetcher.cpp)
target_link_libraries(prefetcher blackhole_thing)

add_executable(fivetwelve fivetwelve.cpp)
target_link_libraries(fivetwelve blackhole_thing)

add_executable(mmio_bench mmio_bench.cpp)
target_link_libraries(mmio_bench blackhole_thing)