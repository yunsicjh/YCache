if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/benchmark/CMakeLists.txt")
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/benchmark benchmark_build EXCLUDE_FROM_ALL)
else()
    find_package(benchmark REQUIRED)
endif()

add_executable(bench_cache bench_cache.cpp)
target_link_libraries(bench_cache PRIVATE
    YsCache
    benchmark::benchmark
)

# 确保 Release 优化级别
target_compile_options(bench_cache PRIVATE -O3)
