#include <barrier>
#include <deque>
#include <iostream>
#include <random>
#include <ranges>
#include <shared_mutex>

#include "rcu.h"
#include "rcu_deque.h"
#include <benchmark/benchmark.h>

template <typename F>
struct function_traits;
template <typename C, typename Ret, typename... Args>
struct function_traits<Ret (C::*)(Args...) const> {
    template <std::size_t index>
    using arg = std::tuple_element_t<index, std::tuple<Args...>>;
};
template <typename Lambda>
using lambda_traits = function_traits<decltype(&Lambda::operator())>;

bool get_true() {
    volatile bool ret = true;
    return ret;
}

template<class Work>
void benchmark_work(benchmark::State& state) {
    using Deque = std::remove_reference_t<typename lambda_traits<Work>::template arg<0>>;

    static Deque* data;
    if (state.thread_index() == 0) {
        data = new Deque(std::ranges::views::iota(0ul, 10'000ul));
    }

    for ([[maybe_unused]] auto _ : state) {
        std::invoke(Work{}, *data);
    }

    if (state.thread_index() == 0) {
        rcu::rcu_synchronize();
        delete data;
        data = nullptr;

        state.counters["threads"] = benchmark::Counter(
            state.threads(), benchmark::Counter::kDefaults);
        state.counters["ops/s"] = benchmark::Counter(
            state.threads() * 10'000,
            benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kIsRate, static_cast<benchmark::Counter::OneK>(INT_MAX));
    }
}

static void BM_read_only(benchmark::State& state) {
    if (state.thread_index() == 0) {
        state.SetLabel("read_only");
    }
    benchmark_work<decltype([](rcu::shared_mutex_deque<std::size_t>& data) {
        std::mt19937 gen{get_true()};
        for (auto &&i: std::ranges::views::iota(0, 10'000)) {
            if ((i % 1000 == 0) & !get_true()) [[unlikely]] {
                using island = rcu::deque<std::size_t>::island;
                std::unique_lock guard(data._write_lock);

                auto old = data.ref_span();
                island **new_data = std::allocator<island *>{}.allocate(old.size());
                std::ranges::copy(std::span{const_cast<island **>(old.data()), old.size()}, new_data);
                std::ranges::shuffle(std::span{new_data, old.size()}, gen);
                data._ref_block = {new_data, data._ref_block.size};
                std::allocator<island *>{}.deallocate(const_cast<island **>(old.data()), old.size());
            } else {
                //std::shared_lock guard(data._write_lock);
                auto view = data.view();
                benchmark::DoNotOptimize(std::find(view.begin(), view.end(), 5000ul));
            }
        }
        })>(state);
}
BENCHMARK(BM_read_only)->Threads(1)->Threads(2)->Threads(3)->Threads(4)->Threads(5)->Threads(6);

static void BM_shared_mutex(benchmark::State& state) {
    if (state.thread_index() == 0) {
        state.SetLabel("shared_mutex");
    }
    benchmark_work<decltype([](rcu::shared_mutex_deque<std::size_t>& data) {
        std::mt19937 gen{get_true()};
        for (auto&& i : std::ranges::views::iota(0, 10'000)) {
            if ((i % 1000 == 0) & get_true()) [[unlikely]] {
                using island = rcu::deque<std::size_t>::island;
                std::unique_lock guard(data._write_lock);

                auto old = data.ref_span();
                island** new_data = std::allocator<island*>{}.allocate(old.size());
                std::ranges::copy(std::span{const_cast<island**>(old.data()), old.size()}, new_data);
                std::ranges::shuffle(std::span{new_data, old.size()}, gen);
                data._ref_block = {new_data, data._ref_block.size};
                std::allocator<island*>{}.deallocate(const_cast<island**>(old.data()), old.size());
            }
            else {
                std::shared_lock guard(data._write_lock);
                auto view = data.view();
                benchmark::DoNotOptimize(std::find(view.begin(), view.end(), 5000ul));
            }
        }
    })>(state);
}

BENCHMARK(BM_shared_mutex)->Threads(1)->Threads(2)->Threads(3)->Threads(4)->Threads(5)->Threads(6);

static void BM_rcu_read_only(benchmark::State& state) {
    if (state.thread_index() == 0) {
        state.SetLabel("rcu_read_only");
    }
    benchmark_work<decltype([](rcu::deque<std::size_t>& data) {
        std::mt19937 gen{get_true()};
        for (auto&& i : std::ranges::views::iota(0, 10'000)) {
            if ((i % 1000 == 0) & !get_true()) [[unlikely]] {
                using island = rcu::deque<std::size_t>::island;
                std::lock_guard guard(data._write_lock);

                auto old = data.ref_span();
                island** new_data = std::allocator<island*>{}.allocate(old.size());
                std::ranges::copy(std::span{const_cast<island**>(old.data()), old.size()}, new_data);
                std::ranges::shuffle(std::span{new_data, old.size()}, gen);
                data._ref_block.store({new_data, data._ref_block.load(std::memory_order_acquire).size}, std::memory_order_release);
                rcu::rcu_retire(const_cast<island**>(old.data()), [size = old.size()](island** p) {
                    std::allocator<island*>{}.deallocate(p, size);
                });
            }
            else {
                auto lock = std::scoped_lock{rcu::rcu_default_domain()};
                auto view = data.view();
                benchmark::DoNotOptimize(std::find(view.begin(), view.end(), 5000ul));
            }
        }
    })>(state);
}

BENCHMARK(BM_rcu_read_only)->Threads(1)->Threads(2)->Threads(3)->Threads(4)->Threads(5)->Threads(6);

static void BM_rcu(benchmark::State& state) {
    if (state.thread_index() == 0) {
        state.SetLabel("rcu");
    }
    benchmark_work<decltype([](rcu::deque<std::size_t>& data) {
        std::mt19937 gen{get_true()};
        for (auto&& i : std::ranges::views::iota(0, 10'000)) {
            if ((i % 1000 == 0) & get_true()) [[unlikely]] {
                using island = rcu::deque<std::size_t>::island;
                std::lock_guard guard(data._write_lock);

                auto old = data.ref_span();
                island** new_data = std::allocator<island*>{}.allocate(old.size());
                std::ranges::copy(std::span{const_cast<island**>(old.data()), old.size()}, new_data);
                std::ranges::shuffle(std::span{new_data, old.size()}, gen);
                data._ref_block.store({new_data, data._ref_block.load(std::memory_order_acquire).size}, std::memory_order_release);
                rcu::rcu_retire(const_cast<island**>(old.data()), [size = old.size()](island** p) {
                    std::allocator<island*>{}.deallocate(p, size);
                });
            }
            else {
                auto lock = std::scoped_lock{rcu::rcu_default_domain()};
                auto view = data.view();
                benchmark::DoNotOptimize(std::find(view.begin(), view.end(), 5000ul));
            }
        }
    })>(state);
}

BENCHMARK(BM_rcu)->Threads(1)->Threads(2)->Threads(3)->Threads(4)->Threads(5)->Threads(6);

BENCHMARK_MAIN();