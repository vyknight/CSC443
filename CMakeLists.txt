cmake_minimum_required(VERSION 3.22)
project(KVDatabase)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g -fno-omit-frame-pointer")

add_compile_options(-O2 -Wall -Wextra)
add_executable(KVDatabase
    main.cpp
    avl_tree.cpp
    newsstable.cpp
    kvdb.cpp
)