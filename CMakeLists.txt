cmake_minimum_required(VERSION 4.0)
project(rcu)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_FLAGS "-Wall -Wextra -O3 -march=native")

add_executable(rcu main.cpp
        rcu.h
        rcu_deque.h
        rcu.cpp)

find_package(benchmark REQUIRED)
target_link_libraries(rcu benchmark::benchmark atomic)