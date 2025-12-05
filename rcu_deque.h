#ifndef RCU_LINKED_LIST_H
#define RCU_LINKED_LIST_H
#include <cassert>
#include <cstring>

namespace rcu {
    template<class T>
    struct atomic_vector : std::ranges::range_adaptor_closure<atomic_vector<T>> {
        struct ptr_size {
            T* _ptr;
            std::size_t _size;
        };

        std::atomic<ptr_size> _data;

        template<class R>
        explicit atomic_vector(const R& range) : _data{{std::allocator<T>{}.allocate(range.size()), range.size()}} {
            std::ranges::copy(range, _data.load(std::memory_order_acquire)._ptr);
        }
        atomic_vector() : _data{nullptr, 0} {}
        atomic_vector(const atomic_vector& rhs) : _data{} {
            const auto to_copy = rhs._data.load(std::memory_order_acquire);
            _data.store({std::allocator<T>{}.allocate(to_copy._size), to_copy._size}, std::memory_order_release);
            std::memcpy(_data.load(std::memory_order_acquire)._ptr, to_copy._ptr, sizeof(T) * to_copy._size);
        };
        atomic_vector(atomic_vector&& rhs)  noexcept : _data{std::exchange(rhs._data.load(std::memory_order_acquire), nullptr)} {}
        atomic_vector& operator=(const atomic_vector& rhs) {
            if (&rhs != this) {
                const auto to_copy = rhs._data.load(std::memory_order_acquire);
                auto old = _data.exchange({std::allocator<T>{}.allocate(to_copy._size), to_copy._size}, std::memory_order_release);
                std::memcpy(_data.load(std::memory_order_acquire)._ptr, to_copy._ptr, sizeof(T) * to_copy._size);
                std::allocator<T>{}.deallocate(old._ptr, old._size);
            }
            return *this;
        }
        atomic_vector& operator=(atomic_vector&& rhs)  noexcept {
            auto old = _data.exchange(rhs._data.exchange({nullptr, 0}, std::memory_order_release), std::memory_order_release);
            std::allocator<T>{}.deallocate(old._ptr, old._size);
            return *this;
        }
        ~atomic_vector() noexcept {
            std::allocator<T>{}.deallocate(_data.load(std::memory_order_relaxed)._ptr, _data.load(std::memory_order_relaxed)._size);
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return _data.load(std::memory_order_acquire)._size;
        }

        //NOT THREAD SAFE
        void push_back(T val) {
            const auto span = get_span();
            auto new_data = std::allocator<T>{}.allocate(span.size() + 1);
            std::ranges::copy(span, new_data);
            new_data[span.size()] = val;
            auto old = _data.exchange({new_data, span.size() + 1});
            std::allocator<T>{}.deallocate(old._ptr, old._size);
        }
        auto get_span() noexcept {
            const auto data = _data.load(std::memory_order_acquire);
            return std::span<T>(data._ptr, data._size);
        }
    };

    template<class T>
    struct deque {
        static constexpr std::size_t island_size = 256;
        using island = std::array<T, island_size>;

        struct iter {
            using iterator_category = std::random_access_iterator_tag;
            using value_type = T;
            using difference_type = std::ptrdiff_t;
            using pointer = T*;
            using reference = T&;

            const island* const * data;
            std::size_t index;
            auto operator++(int) {
                auto tmp = *this;
                operator++();
                return tmp;
            }
            auto operator--(int) {
                auto tmp = *this;
                operator--();
                return tmp;
            }

            auto& operator++() {
                index++;
                return *this;
            }
            auto& operator--() {
                index--;
                return *this;
            }
            const value_type& operator*() {
                return (*(data[index / island_size]))[index % island_size];
            }
            const value_type* operator->() {
                return &**this;
            }
            auto& operator[](const std::size_t pos) {
                return *(iter{data, index + pos});
            }
            friend difference_type operator-(const iter& lhs, const iter& rhs) {
                return lhs.index - rhs.index;
            }
            bool operator==(const iter& rhs) const noexcept = default;
            bool operator!=(const iter& rhs) const noexcept = default;
        };
        struct view_t : std::ranges::view_interface<view_t> {
            std::span<const island* const> data;
            explicit view_t(std::span<const island* const>  data) : data(data) {}
            view_t() = default;

            auto begin() const noexcept {
                return iter{data.data(), 0};
            }
            auto end() const noexcept {
                return iter{data.data(), island_size * size()};
            }
            auto size() const noexcept {
                return island_size * data.size();
            }
        };

        struct ref_block_t {
            const island* const * ptr;
            std::size_t size;
        };
        std::mutex _write_lock;
        std::atomic<ref_block_t> _ref_block;

        deque() : _write_lock{}, _ref_block{} {}
        template<class R>
        explicit deque(R&& rg) : _write_lock{}, _ref_block{{nullptr, 0}} {
            const auto size = rg.size();
            if (size) {
                const auto block_size = (size + island_size - 1) / island_size;
                island** ptr = std::allocator<island*>{}.allocate(block_size);
                for (auto&& [i, p] : std::ranges::views::zip(rg | std::ranges::views::chunk(island_size), std::span{ptr, block_size})) {
                    p = new island{};
                    std::ranges::copy(i, p->data());
                }
                _ref_block.store({ptr, size}, std::memory_order_release);
            }
        }
        deque(deque const&) = delete;
        deque(deque&&) = delete;
        deque& operator=(deque const&) = delete;
        deque& operator=(deque&&) = delete;
        ~deque() {
            auto block = ref_span();
            for (auto&& i : block) {
                delete i;
            }
            std::allocator<island*>{}.deallocate(const_cast<island**>(block.data()), block.size());
        }

        iter begin(std::span<const island* const>  span) {
            return iter{span.data(), 0};
        }
        iter end(std::span<const island* const>  span) {
            return iter{span.data(), span.size()};
        }
        auto ref_span() noexcept {
            auto block = _ref_block.load(std::memory_order_acquire);
            return std::span<const island* const>{block.ptr, block.size == 0 ? 0 : (block.size + island_size - 1) / island_size};
        }
        auto view() noexcept {
            return view_t{ref_span()};
        }
    };

    template<class T>
    struct shared_mutex_deque {
        static constexpr std::size_t island_size = 256;
        using island = std::array<T, island_size>;

        struct iter {
            using iterator_category = std::random_access_iterator_tag;
            using value_type = T;
            using difference_type = std::ptrdiff_t;
            using pointer = T*;
            using reference = T&;

            const island* const * data;
            std::size_t index;
            auto operator++(int) {
                auto tmp = *this;
                operator++();
                return tmp;
            }
            auto operator--(int) {
                auto tmp = *this;
                operator--();
                return tmp;
            }

            auto& operator++() {
                index++;
                return *this;
            }
            auto& operator--() {
                index--;
                return *this;
            }
            const value_type& operator*() {
                return (*(data[index / island_size]))[index % island_size];
            }
            const value_type* operator->() {
                return &**this;
            }
            auto& operator[](const std::size_t pos) {
                return *(iter{data, index + pos});
            }
            friend difference_type operator-(const iter& lhs, const iter& rhs) {
                return lhs.index - rhs.index;
            }
            bool operator==(const iter& rhs) const noexcept = default;
            bool operator!=(const iter& rhs) const noexcept = default;
        };
        struct view_t : std::ranges::view_interface<view_t> {
            std::span<const island* const> data;
            explicit view_t(std::span<const island* const>  data) : data(data) {}
            view_t() = default;

            auto begin() const noexcept {
                return iter{data.data(), 0};
            }
            auto end() const noexcept {
                return iter{data.data(), island_size * size()};
            }
            auto size() const noexcept {
                return island_size * data.size();
            }
        };

        struct ref_block_t {
            const island* const * ptr;
            std::size_t size;
        };
        std::shared_mutex _write_lock;
        ref_block_t _ref_block;

        shared_mutex_deque() : _write_lock{}, _ref_block{} {}
        template<class R>
        explicit shared_mutex_deque(R&& rg) : _write_lock{}, _ref_block{nullptr, 0} {
            const auto size = rg.size();
            if (size) {
                const auto block_size = (size + island_size - 1) / island_size;
                island** ptr = std::allocator<island*>{}.allocate(block_size);
                for (auto&& [i, p] : std::ranges::views::zip(rg | std::ranges::views::chunk(island_size), std::span{ptr, block_size})) {
                    p = new island{};
                    std::ranges::copy(i, p->data());
                }
                _ref_block = {ptr, size};
            }
        }
        shared_mutex_deque(shared_mutex_deque const&) = delete;
        shared_mutex_deque(shared_mutex_deque&&) = delete;
        shared_mutex_deque& operator=(shared_mutex_deque const&) = delete;
        shared_mutex_deque& operator=(shared_mutex_deque&&) = delete;
        ~shared_mutex_deque() {
            auto block = ref_span();
            for (auto&& i : block) {
                delete i;
            }
            std::allocator<island*>{}.deallocate(const_cast<island**>(block.data()), block.size());
        }

        iter begin(std::span<const island* const>  span) {
            return iter{span.data(), 0};
        }
        iter end(std::span<const island* const>  span) {
            return iter{span.data(), span.size()};
        }
        auto ref_span() noexcept {
            auto& block = _ref_block;
            return std::span<const island* const>{block.ptr, block.size == 0 ? 0 : (block.size + island_size - 1) / island_size};
        }
        auto view() noexcept {
            return view_t{ref_span()};
        }
    };
}


#endif //RCU_LINKED_LIST_H