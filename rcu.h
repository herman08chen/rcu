#ifndef RCU_RCU_H
#define RCU_RCU_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <ranges>
#include <span>
#include <thread>
#include <utility>

namespace rcu {

// ---------------------------------------------------------------------------
// Type-erased, move-only deleter.
//
// `rcu_retire` stores a callable that reclaims a retired pointer. The callable
// is type-erased with std::move_only_function (C++23), which is move-only and
// avoids the manual small-buffer/vtable machinery of the previous
// implementation. Deleters are expected to be non-throwing (delete and typical
// custom deleters are noexcept).
// ---------------------------------------------------------------------------

namespace detail {

// Sharded reference counts used by an RCU domain to track active readers.
//
// `N`   : number of ref-count slots (readers hash to one slot).
// `PadBytes`: padding appended to each atomic slot so that distinct slots do
//             not share a cache line (false sharing). Set to 0 to pack slots
//             together (deliberately demonstrates false sharing).
template <std::size_t N, std::size_t PadBytes>
struct padded_sharded_ref_counts {
    static_assert(N >= 1, "at least one ref-count slot is required");

    std::array<std::pair<std::atomic<std::size_t>, std::array<std::byte, PadBytes>>,
                N>
        slots{};

    [[nodiscard]] static constexpr std::size_t count() noexcept { return N; }

    [[nodiscard]] std::atomic<std::size_t>& at(std::size_t index) noexcept {
        return slots[index].first;
    }

    [[nodiscard]] auto refs() noexcept {
        return slots | std::ranges::views::keys;
    }
};

// A batch of pointers awaiting reclamation, plus the reader reference counts
// that must reach zero before the batch can be freed.
template <class RefCounts>
class garbage_queue {
  public:
    static constexpr std::size_t capacity = 512;
    using entry_t = std::pair<void*, std::move_only_function<void(void*)>>;

    garbage_queue() = default;

    [[nodiscard]] RefCounts& ref_counts() noexcept { return ref_counts_; }

    // Wait until every reader that may reference this generation is done.
    // The acquire load pairs with readers' release store in unlock(), so
    // observing zero synchronizes with the end of every read-side critical
    // section that started before this point. Using relaxed loads here would
    // break that happens-before and let a writer reclaim memory a reader is
    // still accessing.
    void synchronize() noexcept {
        for (auto& counter : ref_counts_.refs()) {
            while (counter.load(std::memory_order_acquire) != 0) {
                std::this_thread::yield();
            }
        }
    }

    [[nodiscard]] bool try_synchronize() noexcept {
        return std::ranges::all_of(ref_counts_.refs(), [](auto& counter) {
            return counter.load(std::memory_order_acquire) == 0;
        });
    }

    [[nodiscard]] bool try_push(entry_t&& entry) noexcept {
        if (size_ < capacity) [[likely]] {
            queue_[size_++] = std::move(entry);
            return true;
        }
        return false;
    }

    void push_unchecked(entry_t&& entry) noexcept {
        queue_[size_++] = std::move(entry);
    }

    // Reclaim every retired pointer in the queue. Callers must guarantee no
    // reader can still reference them (e.g. after synchronize()).
    void clear() noexcept {
        for (auto& [ptr, deleter] : std::span{queue_.data(), size_}) {
            deleter(ptr);
        }
        size_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  private:
    RefCounts ref_counts_{};
    std::array<entry_t, capacity> queue_{};
    std::size_t size_ = 0;
};

} // namespace detail

// ---------------------------------------------------------------------------
// Read-copy-update domain.
//
// Two generations of garbage are kept. Readers announce themselves in the
// generation that is current when they lock(); a writer reclaims a generation
// only after all its readers have unlocked(). retire() hands ownership of a
// pointer to the domain; the pointer is freed once a grace period has elapsed.
// ---------------------------------------------------------------------------
template <class RefCounts>
class rcu_domain {
  public:
    rcu_domain() = default;
    ~rcu_domain() = default;

    rcu_domain(const rcu_domain&) = delete;
    rcu_domain& operator=(const rcu_domain&) = delete;
    rcu_domain(rcu_domain&&) = delete;
    rcu_domain& operator=(rcu_domain&&) = delete;

    // Mark the beginning of a read-side critical section. Nested calls are
    // counted; the underlying ref-count is incremented once per outermost
    // lock()/unlock() pair.
    void lock() noexcept {
        if (reader_depth() == 0) {
            current_counter() = &generation_ref_count();
        }
        ++reader_depth();
        current_counter()->fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] bool try_lock() noexcept {
        lock();
        return true;
    }

    // Mark the end of a read-side critical section.
    void unlock() noexcept {
        if (reader_depth() == 0) {
            // Defensive: an unlock() without a matching lock() is a no-op
            // rather than an underflow.
            return;
        }
        --reader_depth();
        current_counter()->fetch_sub(1, std::memory_order_release);
    }

    // Hand `ptr` to the domain for reclamation. `deleter` is invoked with
    // `ptr` once a grace period has elapsed. Ownership transfers to the
    // domain; do not use `ptr` after calling retire().
    void retire(void* ptr, std::move_only_function<void(void*)> deleter) {
        std::lock_guard guard{mutex_};
        const auto current_gen = generation_.load(std::memory_order_acquire);
        auto entry = std::pair<void*, std::move_only_function<void(void*)>>{
            ptr, std::move(deleter)};
        if (!garbage_[current_gen % 2].try_push(std::move(entry))) [[unlikely]] {
            const auto next_gen = (current_gen + 1) % 2;
            garbage_[next_gen].synchronize();
            garbage_[next_gen].clear();
            garbage_[next_gen].push_unchecked(std::move(entry));
            generation_.store(current_gen + 1, std::memory_order_release);
        }
    }

    // Reclaim one generation's worth of garbage: wait for its readers to
    // drain, free its retired pointers, then advance the generation. Calling
    // half_sync() twice constitutes a full grace period.
    void half_sync() noexcept {
        const auto current_gen = generation_.load(std::memory_order_acquire);
        const auto target_gen = (current_gen + 1) % 2;
        std::unique_lock guard{mutex_};
        if (generation_.load(std::memory_order_acquire) < current_gen + 1) [[likely]] {
            garbage_[target_gen].synchronize();
            garbage_[target_gen].clear();
            generation_.store(current_gen + 1, std::memory_order_release);
        }
    }

  private:
    static std::size_t& key() noexcept {
        thread_local const std::size_t value =
            std::hash<std::thread::id>{}(std::this_thread::get_id()) %
            RefCounts::count();
        return const_cast<std::size_t&>(value);
    }

    static std::uint64_t& reader_depth() noexcept {
        thread_local std::uint64_t value = 0;
        return value;
    }

    static std::atomic<std::size_t>*& current_counter() noexcept {
        thread_local std::atomic<std::size_t>* value = nullptr;
        return value;
    }

    std::atomic<std::size_t>& generation_ref_count() noexcept {
        return garbage_[generation_.load(std::memory_order_acquire) % 2]
            .ref_counts()
            .at(key());
    }

    using garbage_queue_t = detail::garbage_queue<RefCounts>;

    std::atomic<std::size_t> generation_{0};
    std::array<garbage_queue_t, 2> garbage_{};
    std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// v2: 16 sharded reader ref-counts, each padded to its own cache line.
// ---------------------------------------------------------------------------
namespace v2 {
using ref_counts = detail::padded_sharded_ref_counts<16, 64 - sizeof(std::atomic<std::size_t>)>;
using rcu_domain = rcu_domain<ref_counts>;

inline rcu_domain& rcu_default_domain() noexcept {
    static rcu_domain domain;
    return domain;
}

template <class T, class D = std::default_delete<T>>
void rcu_retire(T* ptr, D deleter = D(), rcu_domain& dom = rcu_default_domain()) {
    dom.retire(ptr, [deleter = std::move(deleter)](void* p) mutable {
        std::move(deleter)(static_cast<T*>(p));
    });
}

inline void rcu_synchronize(rcu_domain& dom = rcu_default_domain()) noexcept {
    dom.half_sync();
    dom.half_sync();
}

inline void rcu_barrier(rcu_domain& dom = rcu_default_domain()) noexcept {
    rcu_synchronize(dom);
}
} // namespace v2

// ---------------------------------------------------------------------------
// single_ref_count: a single global reader ref-count (no sharding, maximum
// contention, included for comparison).
// ---------------------------------------------------------------------------
namespace single_ref_count {
using ref_counts = detail::padded_sharded_ref_counts<1, 128>;
using rcu_domain = rcu_domain<ref_counts>;

inline rcu_domain& rcu_default_domain() noexcept {
    static rcu_domain domain;
    return domain;
}

template <class T, class D = std::default_delete<T>>
void rcu_retire(T* ptr, D deleter = D(), rcu_domain& dom = rcu_default_domain()) {
    dom.retire(ptr, [deleter = std::move(deleter)](void* p) mutable {
        std::move(deleter)(static_cast<T*>(p));
    });
}

inline void rcu_synchronize(rcu_domain& dom = rcu_default_domain()) noexcept {
    dom.half_sync();
    dom.half_sync();
}

inline void rcu_barrier(rcu_domain& dom = rcu_default_domain()) noexcept {
    rcu_synchronize(dom);
}
} // namespace single_ref_count

// ---------------------------------------------------------------------------
// false_sharing: 16 sharded reader ref-counts packed with no padding, so
// distinct slots share cache lines (deliberately exhibits false sharing).
// ---------------------------------------------------------------------------
namespace false_sharing {
using ref_counts = detail::padded_sharded_ref_counts<16, 0>;
using rcu_domain = rcu_domain<ref_counts>;

inline rcu_domain& rcu_default_domain() noexcept {
    static rcu_domain domain;
    return domain;
}

template <class T, class D = std::default_delete<T>>
void rcu_retire(T* ptr, D deleter = D(), rcu_domain& dom = rcu_default_domain()) {
    dom.retire(ptr, [deleter = std::move(deleter)](void* p) mutable {
        std::move(deleter)(static_cast<T*>(p));
    });
}

inline void rcu_synchronize(rcu_domain& dom = rcu_default_domain()) noexcept {
    dom.half_sync();
    dom.half_sync();
}

inline void rcu_barrier(rcu_domain& dom = rcu_default_domain()) noexcept {
    rcu_synchronize(dom);
}
} // namespace false_sharing

} // namespace rcu

#endif // RCU_RCU_H
