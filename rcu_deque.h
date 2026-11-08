#ifndef RCU_DEQUE_H
#define RCU_DEQUE_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <shared_mutex>
#include <utility>

namespace rcu {

// A chunked, copy-on-write deque optimized for read-copy-update.
//
// The sequence is stored as an array of fixed-size "islands"
// (std::array<T, island_size>). Readers obtain a snapshot of the island
// pointer array via view() and iterate it without taking any lock. Writers
// allocate a fresh island pointer array, publish it atomically, and retire the
// previous array through an RCU domain so that it is freed only after all
// pre-existing readers have finished.
//
// Not copyable or movable; the caller is responsible for synchronizing access
// and for retiring old generations through RCU.
template <class T>
struct deque {
    static constexpr std::size_t island_size = 256;
    using island = std::array<T, island_size>;

    struct iter {
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        const island* const* data;
        std::size_t index = 0;

        [[nodiscard]] auto operator++(int) {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }
        [[nodiscard]] auto operator--(int) {
            auto tmp = *this;
            --(*this);
            return tmp;
        }

        iter& operator++() {
            ++index;
            return *this;
        }
        iter& operator--() {
            --index;
            return *this;
        }

        [[nodiscard]] const value_type& operator*() const {
            return (*(data[index / island_size]))[index % island_size];
        }
        [[nodiscard]] const value_type* operator->() const {
            return &**this;
        }

        [[nodiscard]] reference operator[](std::size_t pos) {
            return *(iter{data, index + pos});
        }

        [[nodiscard]] friend difference_type operator-(const iter& lhs,
                                                       const iter& rhs) {
            return static_cast<difference_type>(lhs.index - rhs.index);
        }
        [[nodiscard]] friend bool operator==(const iter& lhs,
                                             const iter& rhs) noexcept {
            return lhs.index == rhs.index;
        }
        [[nodiscard]] friend bool operator!=(const iter& lhs,
                                             const iter& rhs) noexcept {
            return lhs.index != rhs.index;
        }
    };

    // A snapshot of the island pointer array. Iterating a view does not
    // synchronize with writers; the caller must hold an RCU read lock for the
    // duration of its use.
    struct view_t : std::ranges::view_interface<view_t> {
        std::span<const island* const> data;

        view_t() = default;
        explicit view_t(std::span<const island* const> data) : data(data) {}

        [[nodiscard]] iter begin() const noexcept {
            return iter{data.data(), 0};
        }
        [[nodiscard]] iter end() const noexcept {
            // One past the last valid element. `size()` is the total number of
            // elements (rounded up to an island boundary), so this never
            // indexes past the allocated island pointer array.
            return iter{data.data(), size()};
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return island_size * data.size();
        }
    };

    struct ref_block_t {
        const island* const* ptr;
        std::size_t size; // number of elements
    };

    std::mutex _write_lock;
    std::atomic<ref_block_t> _ref_block;

    deque() : _write_lock{}, _ref_block{{nullptr, 0}} {}

    template <class R>
    explicit deque(R&& rg) : _write_lock{}, _ref_block{{nullptr, 0}} {
        const auto size = rg.size();
        if (size == 0) {
            return;
        }
        const auto block_size = (size + island_size - 1) / island_size;
        island** const ptr = std::allocator<island*>{}.allocate(block_size);
        std::size_t block = 0;
        for (auto&& chunk :
             rg | std::ranges::views::chunk(island_size)) {
            island* const p = new island{};
            std::ranges::copy(chunk, p->data());
            ptr[block++] = p;
        }
        _ref_block.store({ptr, size}, std::memory_order_release);
    }

    deque(const deque&) = delete;
    deque(deque&&) = delete;
    deque& operator=(const deque&) = delete;
    deque& operator=(deque&&) = delete;

    ~deque() {
        const auto block = ref_span();
        for (const island* const* i = block.data();
             i != block.data() + block.size(); ++i) {
            delete *i;
        }
        std::allocator<island*>{}.deallocate(
            const_cast<island**>(block.data()), block.size());
    }

    [[nodiscard]] auto ref_span() noexcept {
        const auto block = _ref_block.load(std::memory_order_acquire);
        const auto block_count =
            block.size == 0
                ? 0
                : (block.size + island_size - 1) / island_size;
        return std::span<const island* const>{block.ptr, block_count};
    }

    [[nodiscard]] auto view() noexcept { return view_t{ref_span()}; }
};

// A deque whose current generation is protected by a std::shared_mutex rather
// than RCU. Readers take a shared lock; writers take the exclusive lock and
// replace the island pointer array in place. Useful as a baseline for
// comparison against the RCU deque.
template <class T>
struct shared_mutex_deque {
    static constexpr std::size_t island_size = 256;
    using island = std::array<T, island_size>;

    struct iter {
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        const island* const* data;
        std::size_t index = 0;

        [[nodiscard]] auto operator++(int) {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }
        [[nodiscard]] auto operator--(int) {
            auto tmp = *this;
            --(*this);
            return tmp;
        }

        iter& operator++() {
            ++index;
            return *this;
        }
        iter& operator--() {
            --index;
            return *this;
        }

        [[nodiscard]] const value_type& operator*() const {
            return (*(data[index / island_size]))[index % island_size];
        }
        [[nodiscard]] const value_type* operator->() const {
            return &**this;
        }

        [[nodiscard]] reference operator[](std::size_t pos) {
            return *(iter{data, index + pos});
        }

        [[nodiscard]] friend difference_type operator-(const iter& lhs,
                                                       const iter& rhs) {
            return static_cast<difference_type>(lhs.index - rhs.index);
        }
        [[nodiscard]] friend bool operator==(const iter& lhs,
                                             const iter& rhs) noexcept {
            return lhs.index == rhs.index;
        }
        [[nodiscard]] friend bool operator!=(const iter& lhs,
                                             const iter& rhs) noexcept {
            return lhs.index != rhs.index;
        }
    };

    struct view_t : std::ranges::view_interface<view_t> {
        std::span<const island* const> data;

        view_t() = default;
        explicit view_t(std::span<const island* const> data) : data(data) {}

        [[nodiscard]] iter begin() const noexcept {
            return iter{data.data(), 0};
        }
        [[nodiscard]] iter end() const noexcept {
            return iter{data.data(), size()};
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return island_size * data.size();
        }
    };

    struct ref_block_t {
        const island* const* ptr;
        std::size_t size; // number of elements
    };

    std::shared_mutex _write_lock;
    ref_block_t _ref_block;

    shared_mutex_deque() : _write_lock{}, _ref_block{nullptr, 0} {}

    template <class R>
    explicit shared_mutex_deque(R&& rg) : _write_lock{}, _ref_block{nullptr, 0} {
        const auto size = rg.size();
        if (size == 0) {
            return;
        }
        const auto block_size = (size + island_size - 1) / island_size;
        island** const ptr = std::allocator<island*>{}.allocate(block_size);
        std::size_t block = 0;
        for (auto&& chunk :
             rg | std::ranges::views::chunk(island_size)) {
            island* const p = new island{};
            std::ranges::copy(chunk, p->data());
            ptr[block++] = p;
        }
        _ref_block = {ptr, size};
    }

    shared_mutex_deque(const shared_mutex_deque&) = delete;
    shared_mutex_deque(shared_mutex_deque&&) = delete;
    shared_mutex_deque& operator=(const shared_mutex_deque&) = delete;
    shared_mutex_deque& operator=(shared_mutex_deque&&) = delete;

    ~shared_mutex_deque() {
        const auto block = ref_span();
        for (const island* const* i = block.data();
             i != block.data() + block.size(); ++i) {
            delete *i;
        }
        std::allocator<island*>{}.deallocate(
            const_cast<island**>(block.data()), block.size());
    }

    [[nodiscard]] auto ref_span() noexcept {
        const auto& block = _ref_block;
        const auto block_count =
            block.size == 0
                ? 0
                : (block.size + island_size - 1) / island_size;
        return std::span<const island* const>{block.ptr, block_count};
    }

    [[nodiscard]] auto view() noexcept { return view_t{ref_span()}; }
};

} // namespace rcu

#endif // RCU_DEQUE_H
