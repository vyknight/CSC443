cmake_minimum_required(VERSION 3.14)
project(test_bufferpool)

set(CMAKE_CXX_STANDARD 20)

# ---- Fetch GoogleTest ----
include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)

# no installation of gtest
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# ---- Add executable ----
add_executable(test_bufferpool test_bufferpool.cpp ../buffer_pool.cpp)

# ---- Link gtest ----
target_link_libraries(test_bufferpool gtest gtest_main pthread)
