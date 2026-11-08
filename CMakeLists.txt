cmake_minimum_required(VERSION 3.28)
project(rcu LANGUAGES CXX)

# CMake 3.28 does not ship a CXX26 dialect descriptor for GCC, but the
# installed g++-15 accepts -std=c++26 directly. Pass the flag explicitly so the
# project compiles as C++26 with the available compiler.
add_compile_options(-std=c++26 -Wall -Wextra -Wconversion -Werror -O3 -march=native)

find_package(Threads REQUIRED)

# Google Benchmark is an optional third-party dependency. The RCU library and
# its tests build without it; the benchmarking executable only exists when
# benchmark is installed.
find_package(benchmark QUIET)

set(rcu_sources
    main.cpp
    rcu.cpp
    rcu.h
    rcu_deque.h
)

if (benchmark_FOUND)
    add_executable(rcu ${rcu_sources})
    target_link_libraries(rcu PRIVATE benchmark::benchmark Threads::Threads atomic)
else ()
    message(STATUS "benchmark not found: skipping the 'rcu' benchmark executable")
endif ()

add_executable(rcu_tests test_rcu.cpp rcu.h rcu_deque.h rcu.cpp)
target_link_libraries(rcu_tests PRIVATE Threads::Threads atomic)

enable_testing()
add_test(NAME rcu_tests COMMAND rcu_tests)
