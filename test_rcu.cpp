#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "rcu.h"
#include "rcu_deque.h"

namespace {

std::size_t g_tests_run = 0;
std::size_t g_tests_failed = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_tests_run;                                                             \
    if (!(cond)) {                                                             \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond        \
                << "\n";                                                       \
      ++g_tests_failed;                                                        \
    }                                                                          \
  } while (0)

// An object whose lifetime we can track through RCU retirement. Its destructor
// poisons `magic` so that any reader observing it after reclamation is
// detected as a use-after-free.
struct tracked {
    std::atomic<std::size_t>* live;
    std::atomic<int> magic;
    std::size_t id;

    tracked(std::atomic<std::size_t>* l, int m, std::size_t i)
        : live(l), magic(m), id(i) {
        if (live) {
            live->fetch_add(1, std::memory_order_relaxed);
        }
    }
    ~tracked() {
        magic.store(-1, std::memory_order_release);
        if (live) {
            live->fetch_sub(1, std::memory_order_relaxed);
        }
    }
};

// ---------------------------------------------------------------------------
// Generic tests: they only use the public domain API, so they run against
// every RCU domain implementation.
// ---------------------------------------------------------------------------
template <class Domain>
void test_basic_reclaim(Domain& dom) {
    std::atomic<std::size_t> live{0};
    {
        tracked* p = new tracked(&live, 0xDEAD, 1);
        CHECK(live.load(std::memory_order_acquire) == std::size_t{1});
        dom.retire(p, [](void* pp) { delete static_cast<tracked*>(pp); });
        // Not reclaimed until a grace period elapses.
        CHECK(live.load(std::memory_order_acquire) == std::size_t{1});
        dom.half_sync();
        dom.half_sync();
        CHECK(live.load(std::memory_order_acquire) == std::size_t{0});
    }
}

template <class Domain>
void test_no_reclaim_before_synchronize(Domain& dom) {
    std::atomic<std::size_t> live{0};
    tracked* p = new tracked(&live, 0, 0);
    CHECK(live.load(std::memory_order_acquire) == std::size_t{1});
    dom.retire(p, [](void* pp) { delete static_cast<tracked*>(pp); });
    CHECK(live.load(std::memory_order_acquire) == std::size_t{1});
    dom.half_sync();
    dom.half_sync();
    CHECK(live.load(std::memory_order_acquire) == std::size_t{0});
}

template <class Domain>
void test_many_retries(Domain& dom) {
    std::atomic<std::size_t> live{0};
    // Retire more objects than a single garbage queue can buffer (capacity 512
    // per generation, two generations) while reclaiming periodically.
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 600; ++i) {
            dom.retire(new tracked(&live, 0, 0),
                       [](void* pp) { delete static_cast<tracked*>(pp); });
        }
        dom.half_sync();
        dom.half_sync();
        CHECK(live.load(std::memory_order_acquire) == std::size_t{0});
    }
}

template <class Domain>
void test_concurrent_reader_writer(Domain& dom) {
    std::atomic<tracked*> current{nullptr};
    std::atomic<std::size_t> live{0};
    std::atomic<bool> stop{false};
    std::atomic<int> use_after_free{0};

    constexpr int magic = 0xDEAD;
    constexpr int writer_iterations = 1000;
    constexpr int reader_iterations = 3000;

    std::thread writer([&]() {
        tracked* prev = nullptr;
        for (int i = 0; i < writer_iterations && !stop.load(std::memory_order_acquire);
             ++i) {
            tracked* p = new tracked(&live, magic, static_cast<std::size_t>(i));
            prev = current.exchange(p, std::memory_order_acq_rel);
            if (prev) {
                dom.retire(prev,
                           [](void* pp) { delete static_cast<tracked*>(pp); });
            }
            // Reclaim periodically while readers are active, so readers that
            // have just released an object can observe it being freed.
            if (i % 64 == 0) {
                dom.half_sync();
                dom.half_sync();
            }
        }
        tracked* final = current.exchange(nullptr, std::memory_order_acq_rel);
        if (final) {
            dom.retire(final,
                       [](void* pp) { delete static_cast<tracked*>(pp); });
        }
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            for (int i = 0; i < reader_iterations && !stop.load(std::memory_order_acquire);
                 ++i) {
                dom.lock();
                tracked* p = current.load(std::memory_order_acquire);
                if (p != nullptr &&
                    p->magic.load(std::memory_order_acquire) != magic) {
                    // A reader observed an already-reclaimed object.
                    use_after_free.fetch_add(1, std::memory_order_relaxed);
                }
                dom.unlock();
            }
        });
    }

    writer.join();
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }

    dom.half_sync();
    dom.half_sync();

    CHECK(use_after_free.load(std::memory_order_acquire) == 0);
    CHECK(live.load(std::memory_order_acquire) == std::size_t{0});
}

void test_deque_view() {
    rcu::deque<std::size_t> d(std::ranges::views::iota(0ul, 10'000ul));
    auto view = d.view();

    CHECK(std::find(view.begin(), view.end(), 5000ul) != view.end());
    // An absent value must reach end() without reading past the island array.
    CHECK(std::find(view.begin(), view.end(), 99999ul) == view.end());
}

void test_shared_mutex_deque_view() {
    rcu::shared_mutex_deque<std::size_t> d(
        std::ranges::views::iota(0ul, 10'000ul));
    auto view = d.view();

    CHECK(std::find(view.begin(), view.end(), 5000ul) != view.end());
    CHECK(std::find(view.begin(), view.end(), 99999ul) == view.end());
}

} // namespace

int main() {
    std::cout << "rcu test suite\n";

    // Exercise every RCU domain implementation with the generic tests.
    test_basic_reclaim(rcu::v2::rcu_default_domain());
    test_no_reclaim_before_synchronize(rcu::v2::rcu_default_domain());
    test_many_retries(rcu::v2::rcu_default_domain());
    test_concurrent_reader_writer(rcu::v2::rcu_default_domain());

    test_basic_reclaim(rcu::single_ref_count::rcu_default_domain());
    test_no_reclaim_before_synchronize(
        rcu::single_ref_count::rcu_default_domain());
    test_many_retries(rcu::single_ref_count::rcu_default_domain());
    test_concurrent_reader_writer(
        rcu::single_ref_count::rcu_default_domain());

    test_basic_reclaim(rcu::false_sharing::rcu_default_domain());
    test_no_reclaim_before_synchronize(
        rcu::false_sharing::rcu_default_domain());
    test_many_retries(rcu::false_sharing::rcu_default_domain());
    test_concurrent_reader_writer(
        rcu::false_sharing::rcu_default_domain());

    test_deque_view();
    test_shared_mutex_deque_view();

    std::cout << "ran " << g_tests_run << " checks, " << g_tests_failed
              << " failed\n";
    return g_tests_failed == 0 ? 0 : 1;
}
