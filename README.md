# rcu — Read-Copy-Update synchronization library

## Overview

`rcu` is a small, header-centric C++ library implementing read-copy-update (RCU)
synchronization. RCU lets readers access shared data without taking locks while
writers replace the data and retire the old version. Retired memory is reclaimed
only after every pre-existing reader has finished, so readers never see partially
updated or freed memory.

The project also contains microbenchmarks that compare the RCU deque against a
mutex-protected deque and measure the cost of the different RCU domain designs.

## Architecture

The library is split into two headers:

- `rcu.h` — the RCU core: reader/writer registration, deferred reclamation, and
  grace-period synchronization. It provides several domain implementations:
  - `rcu::v2` — 16 sharded reader counters, each padded to its own cache line
    (avoids false sharing).
  - `rcu::single_ref_count` — a single global reader counter (maximum
    contention, included for comparison).
  - `rcu::false_sharing` — 16 packed reader counters with no padding
    (deliberately exhibits false sharing).
- `rcu_deque.h` — two chunked, copy-on-write deques:
  - `rcu::deque` — the RCU deque. Writers publish a new generation atomically
    and retire the previous one through an RCU domain.
  - `rcu::shared_mutex_deque` — the same structure guarded by a mutex, used as a
    baseline.

`main.cpp` holds microbenchmarks for the deque and the domain locking variants.
`test_rcu.cpp` is the test suite. `CMakeLists.txt` drives the build.

## How It Works

A reader marks a read-side critical section with `domain.lock()` and ends it
with `domain.unlock()`. On the first (outermost) lock the reader announces
itself in the generation that is currently active; nested locks just increment
the depth.

A writer hands a pointer to the domain with `domain.retire(ptr, deleter)`. The
domain stores the pointer together with its deleter in the current generation's
garbage queue. A pointer is freed only after a grace period: `half_sync()`
waits for one generation's readers to drain, frees that generation's retired
pointers, and advances the generation. Calling `half_sync()` twice
(`rcu_synchronize()` / `rcu_barrier()`) completes a full grace period, so every
pointer retired before the call is guaranteed to be reclaimed once it returns.

Readers never block writers and writers never block readers; reclamation is
deferred until it is safe. The `rcu::deque` exploits this: a writer allocates a
fresh array of island pointers, copies the data, publishes it atomically, and
retires the old array. Readers grab a snapshot via `view()` and traverse it
lock-free for the duration of their read lock.

The three domain implementations differ only in how reader presence is tracked
(sharded vs. single counter, padded vs. packed), letting you measure the impact
of false sharing and counter contention.

## Dependencies

The only external third-party library is:

- [Google Benchmark](https://github.com/google/benchmark) — microbenchmarking
  framework used by `main.cpp`. It is optional: the library and the test suite
  build without it; the `rcu` benchmark executable is produced only when
  Benchmark is installed.

No other third-party libraries are used.

## Installation and Usage

### Prerequisites

- CMake 3.28 or newer
- A C++26-capable compiler (for example g++ 15 or newer; CMake 3.28 does not
  ship a `CXX26` dialect descriptor for GCC, so the flag is passed explicitly)
- Google Benchmark (optional, only for the `rcu` benchmark executable)

### Build

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-15
cmake --build build -j
```

If Google Benchmark is found, the `rcu` benchmark executable is built;
otherwise it is skipped with a status message. The `rcu_tests` executable is
always built.

### Run the tests

```bash
cd build
ctest
```

or run the binary directly:

```bash
./rcu_tests
```

### Run the benchmarks

```bash
./rcu
```

## Example

```cpp
#include "rcu.h"

// A shared pointer to a widget, protected by RCU.
Widget* current = nullptr;
rcu::v2::rcu_domain& domain = rcu::v2::rcu_default_domain();

// Reader: access the widget without blocking writers.
domain.lock();
Widget* w = current;
if (w) w->use();
domain.unlock();

// Writer: install a new widget and retire the previous one.
Widget* replacement = new Widget{...};
Widget* retired = current;
current = replacement;
if (retired)
    domain.retire(retired, [](void* p) { delete static_cast<Widget*>(p); });

// Reclaim everything retired before this point.
rcu::v2::rcu_synchronize(domain);
```
