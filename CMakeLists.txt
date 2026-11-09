cmake_minimum_required(VERSION 3.15)
project(YsCache VERSION 1.0.0 LANGUAGES CXX)

# 1. 基础配置：默认 C++17 与统一编译行为
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认构建类型设为 Release，确保基准测试与高吞吐压测具备完整的编译器优化（如 -O3）
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type (default Release for performance)" FORCE)
endif()

# 2. 引入系统多线程支持（支撑 std::mutex）
find_package(Threads REQUIRED)

# 3. 声明 Header-only 接口目标及其别名
add_library(YsCache INTERFACE)
add_library(YsCache::YsCache ALIAS YsCache)

# 4. 头文件路径（支持构建与安装阶段解耦）
target_include_directories(YsCache INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# 5. 传递 C++17 特性与线程库依赖
target_compile_features(YsCache INTERFACE cxx_std_17)
target_link_libraries(YsCache INTERFACE Threads::Threads)

# 统一编译器高质量警告开关
if(MSVC)
    target_compile_options(YsCache INTERFACE /W4)
else()
    target_compile_options(YsCache INTERFACE -Wall -Wextra -Wpedantic)
endif()

# 6. 测试与示例构建开关
option(YS_BUILD_TESTS "Build unit tests" ON)
option(YS_BUILD_EXAMPLES "Build benchmark and demo executables" ON)

# 引入示例目录（构建压测基准与演示程序）
if(YS_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
    add_subdirectory(src)
endif()

# 引入测试目录（构建自动化单元测试）
if(YS_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
