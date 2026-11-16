find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.1
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(test_lru_cache test_lru_cache.cpp)
target_link_libraries(test_lru_cache PRIVATE
    YsCache
    GTest::gtest_main
)

add_executable(test_cache_builder test_cache_builder.cpp)
target_link_libraries(test_cache_builder PRIVATE
    YsCache
    GTest::gtest_main
)

add_executable(test_policy_cache test_policy_cache.cpp)
target_link_libraries(test_policy_cache PRIVATE
    YsCache
    GTest::gtest_main
)

# 针对 macOS / Conda 等动态库的 RPATH 兼容性配置
if(TARGET GTest::gtest)
    get_target_property(_gtest_loc GTest::gtest IMPORTED_LOCATION_RELEASE)
    if(NOT _gtest_loc)
        get_target_property(_gtest_loc GTest::gtest IMPORTED_LOCATION)
    endif()
    if(_gtest_loc)
        get_filename_component(_gtest_dir "${_gtest_loc}" DIRECTORY)
        set_target_properties(test_lru_cache PROPERTIES BUILD_RPATH "${_gtest_dir}")
        set_target_properties(test_cache_builder PROPERTIES BUILD_RPATH "${_gtest_dir}")
        set_target_properties(test_policy_cache PROPERTIES BUILD_RPATH "${_gtest_dir}")
    endif()
endif()

include(GoogleTest)
gtest_discover_tests(test_lru_cache)
gtest_discover_tests(test_cache_builder)
gtest_discover_tests(test_policy_cache)

