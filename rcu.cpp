//
// Created by herman on 11/22/25.
//

#include "rcu.h"

namespace rcu {
    thread_local const std::size_t rcu_domain::key = std::hash<std::thread::id>{}(std::this_thread::get_id()) % num_ref_counts;

    thread_local std::uint64_t rcu_domain::num_readers = 0;
    thread_local std::atomic<std::size_t>* rcu_domain::counter = &rcu_default_domain().garbage[rcu_default_domain().generation.load(std::memory_order_acquire) % max_gens].ref_count()[key];
}