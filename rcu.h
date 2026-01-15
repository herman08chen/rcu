#ifndef RCU_RCU_H
#define RCU_RCU_H
#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>

namespace rcu {
namespace v1 {
    struct deleter_t {
        template<class D>
        static constexpr bool SBO = sizeof(D) <= sizeof(void*) && alignof(D) <= alignof(void*) && std::is_trivially_copyable_v<D>;

        struct vtable_t {
            std::reference_wrapper<void(std::array<char, sizeof(void*)>*, void*)> invoke_ref;
            std::reference_wrapper<void(std::array<char, sizeof(void*)>)> dealloc_ref;
        };
        template<class T, class D>
        struct vtable {
            static void invoke(std::array<char, sizeof(void*)>* deleter, void* p) {
                if constexpr (!std::is_same_v<D, void>) {
                    if constexpr(SBO<D>) {
                        std::invoke(*reinterpret_cast<D*>(deleter), static_cast<T*>(p));
                    }
                    else {
                        std::invoke(**std::bit_cast<D**>(deleter), static_cast<T*>(p));
                    }
                }
            }
            static void dealloc(std::array<char, sizeof(void*)> deleter) {
                if constexpr (!std::is_same_v<D, void>) {
                    if constexpr(SBO<D>) {
                        reinterpret_cast<D*>(&deleter)->~D();
                    }
                    else {
                        delete std::bit_cast<D*>(deleter);
                    }
                }
            }
            static constexpr vtable_t value = {invoke, dealloc};
        };

        template<class T, class D>
        static constexpr vtable_t vtable_v = vtable<T, D>::value;

        static constexpr vtable_t empty_vtable = vtable<void, void>::value;

        alignas(void*) std::array<char, sizeof(void*)> deleter_ptr;
        std::reference_wrapper<const vtable_t> vtable_ref;

        template<class T, class D>
        constexpr explicit deleter_t(std::type_identity<T>, D d = D()) :
            deleter_ptr{}, vtable_ref{vtable_v<T, D>} {
            if constexpr(SBO<D>) {
                new (static_cast<void*>(&deleter_ptr)) D{std::forward<D>(d)};
            }
            else {
                deleter_ptr = std::bit_cast<decltype(deleter_ptr)>(new D{std::forward<D>(d)});
            }
        }
        constexpr deleter_t() : deleter_ptr{}, vtable_ref{empty_vtable} {}
        constexpr deleter_t(const deleter_t&) = delete;
        constexpr deleter_t(deleter_t&& rhs) noexcept :
            deleter_ptr{std::exchange(rhs.deleter_ptr, std::bit_cast<decltype(deleter_ptr)>(static_cast<void*>(nullptr)))},
            vtable_ref{rhs.vtable_ref} {}
        constexpr deleter_t& operator=(const deleter_t&) = delete;
        constexpr deleter_t& operator=(deleter_t&& rhs) noexcept {
            std::swap(deleter_ptr, rhs.deleter_ptr);
            std::swap(vtable_ref, rhs.vtable_ref);
            return *this;
        };
        constexpr ~deleter_t() noexcept {
            std::invoke(vtable_ref.get().dealloc_ref, deleter_ptr);
        }
        constexpr void operator()(void* p) {
            std::invoke(vtable_ref.get().invoke_ref, &deleter_ptr, p);
        }
        constexpr void clear() noexcept {
            std::invoke(vtable_ref.get().dealloc_ref, deleter_ptr);
            deleter_ptr = std::bit_cast<decltype(deleter_ptr)>(static_cast<void*>(nullptr));
            vtable_ref = empty_vtable;
        }
        [[nodiscard]] constexpr bool is_empty() const noexcept {
            return &vtable_ref.get() == &empty_vtable;
        }
    };

    class rcu_domain;
    rcu_domain& rcu_default_domain() noexcept;

    class rcu_domain {
        static constexpr std::size_t num_ref_counts = 4;
        //static constexpr std::size_t gen_size = 16 * num_ref_counts;
        static constexpr std::size_t max_gens = 2;

        inline thread_local static const std::size_t key = std::hash<std::thread::id>{}(std::this_thread::get_id()) % num_ref_counts;;
        inline thread_local static std::uint64_t num_readers = 0;
        inline thread_local static std::atomic<std::size_t>* counter = nullptr;;

        struct gen_t {
            using ref_count_t = std::atomic<std::size_t>;
            using auto_ptr = std::pair<void*, deleter_t>;
            static constexpr std::size_t num_ptrs_per_group = 64 / sizeof(auto_ptr);
            static constexpr std::size_t ptr_capacity = num_ptrs_per_group * num_ref_counts;
            //using chunk = std::array<auto_ptr, ptr_capacity>;

            struct node {
                auto_ptr data;
                node* next;
            };

            std::array<ref_count_t, num_ref_counts> ref_counts;
            //std::forward_list<chunk> garbage_queue;
            std::atomic<node*> garbage_queue;
            std::atomic<std::size_t> size;
            std::mutex alloc_lock;

            auto& ref_count() noexcept {
                return ref_counts;
            }
            /*auto garbage() noexcept {
                return garbage_queue | std::ranges::views::join;
            }*/

            ~gen_t() noexcept {
                assert(try_synchronize());
                clear();
            }
            void synchronize() noexcept {
                for (auto&& i : ref_count()) {
                    while (i.load(std::memory_order_acquire) != 0) {
                        std::this_thread::yield();
                    }
                }
            }
            bool try_synchronize() noexcept {
                return std::ranges::all_of(ref_count(), [](auto&& count) {
                    return count.load(std::memory_order_acquire) == 0;
                });
            }
            void push(void* ptr, deleter_t&& d) {
                auto* expected = garbage_queue.load(std::memory_order_acquire);
                auto* my_node = new node(auto_ptr{ptr, std::move(d)}, expected);
                while (!garbage_queue.compare_exchange_weak(expected, my_node)) {
                    my_node->next = expected;
                }
                size.fetch_add(1, std::memory_order_release);
            }
            void clear() {
                assert(try_synchronize());
                auto* ptr = garbage_queue.load(std::memory_order_acquire);
                while (ptr) {
                    auto* next = ptr->next;
                    auto&& [p, d] = ptr->data;
                    d(p);
                    p = nullptr;
                    d.clear();
                    delete ptr;
                    ptr = next;
                }
                garbage_queue.store(nullptr, std::memory_order_release);
                size.store(0, std::memory_order_release);
            }
            [[nodiscard]] bool is_full() const {
                return size.load(std::memory_order_acquire) % ptr_capacity == 0;
            }
        };

        std::atomic<std::size_t> generation;
        std::array<gen_t, max_gens> garbage;
        std::mutex cleanup_lock;

        struct default_domain_tag_t {};
        explicit rcu_domain(default_domain_tag_t) : generation{}, garbage{}, cleanup_lock {} {}

        /*auto garbage_queue_view() noexcept {
            return garbage | std::ranges::views::transform([](gen_t& gen) {
                return gen.garbage();
            });
        }*/
        auto ref_count_view() noexcept {
            return garbage | std::ranges::views::transform([](gen_t& gen) -> auto& {
                return gen.ref_count();
            });
        }

    public:
        rcu_domain() = delete;
        rcu_domain(const rcu_domain&) = delete;
        rcu_domain(rcu_domain&&) = delete;
        rcu_domain& operator=(const rcu_domain&) = delete;
        rcu_domain& operator=(rcu_domain&&) = delete;
        ~rcu_domain() noexcept = default;

        void lock() noexcept {
            if (num_readers == 0)
                counter = &garbage[generation.load(std::memory_order_acquire) % max_gens].ref_count()[key];
            num_readers++;
            counter->fetch_add(1, std::memory_order_release);
        }
        bool try_lock() noexcept {
            lock();
            return true;
        }

        void unlock() noexcept {
            [[maybe_unused]] auto _ = static_cast<void*>(this);
            num_readers--;
            counter->fetch_sub(1, std::memory_order_release);
        }

        void retire(void* p, deleter_t&& d) {
            auto current_gen = generation.load(std::memory_order_acquire);
            if (garbage[current_gen % max_gens].is_full() && garbage[(current_gen + 1) % max_gens].try_synchronize() && cleanup_lock.try_lock()) {
                current_gen++;
                generation.store(current_gen, std::memory_order_release);
                garbage[current_gen % max_gens].clear();
                cleanup_lock.unlock();
            }
            garbage[current_gen % max_gens].push(p, std::move(d));
        }
        friend void rcu_synchronize(rcu_domain& dom) noexcept;
        friend rcu_domain& rcu_default_domain() noexcept;
    };
    template<class T, class D = std::default_delete<T>>
    void rcu_retire(T* p, D d = D(), rcu_domain& dom = rcu_default_domain()) {
        dom.retire(static_cast<void*>(p), deleter_t{std::type_identity<T>{}, std::move(d)});
    }
    inline rcu_domain& rcu_default_domain() noexcept {
        static rcu_domain domain{rcu_domain::default_domain_tag_t{}};
        return domain;
    }

    inline void rcu_synchronize(rcu_domain& dom = rcu_default_domain()) noexcept {
        std::lock_guard guard{dom.cleanup_lock};
        dom.generation.fetch_add(1, std::memory_order_release);
        for (auto&& i : dom.garbage) {
            i.synchronize();
            i.clear();
        }
    }

    inline void rcu_barrier(rcu_domain& dom = rcu_default_domain()) noexcept {
        rcu_synchronize(dom);
    }
}
namespace v2 {
    using v1::deleter_t;

    class rcu_domain;
    rcu_domain& rcu_default_domain() noexcept;

    struct garbage_queue {
        static constexpr std::size_t num_ref_counts = 16;
        static constexpr std::size_t capacity = 512;
        using ref_count_t = std::atomic<std::size_t>;
        using padding = std::array<char, 64 - sizeof(ref_count_t)>;
        using auto_ptr = std::pair<void*, deleter_t>;

        std::array<std::pair<ref_count_t, padding>, num_ref_counts> ref_counts;
        std::array<auto_ptr, capacity> queue;
        std::size_t size;

        auto ref_count() noexcept {
            return ref_counts | std::ranges::views::keys;
        }

        garbage_queue() noexcept : ref_counts{}, queue{}, size{} {}
        garbage_queue(const garbage_queue&) = delete;
        garbage_queue(garbage_queue&&) = delete;
        garbage_queue& operator=(const garbage_queue&) = delete;
        garbage_queue& operator=(garbage_queue&&) = delete;
        ~garbage_queue() noexcept {
            assert(try_synchronize());
            clear();
        }
        void synchronize() noexcept {
            for (auto&& i : ref_count()) {
                while (i.load(std::memory_order_relaxed) != 0)
                    std::this_thread::yield();
            }
        }
        bool try_synchronize() noexcept {
            return std::ranges::all_of(ref_count(), [](auto&& count) {
                return count.load(std::memory_order_acquire) == 0;
            });
        }
        bool try_push(auto_ptr&& ptr) noexcept {
            if (size < capacity) [[likely]] {
                const auto index = size++;
                queue[index] = std::move(ptr);
                return true;
            }
            else {
                return false;
            }
        }
        void push_unchecked(auto_ptr&& ptr) noexcept {
            const auto index = size++;
            queue[index] = std::move(ptr);
        }

        void clear() {
            assert(try_synchronize());
            for (auto&& [p, d] : queue | std::ranges::views::take(size)) {
                d(p);
                p = nullptr;
            }
            size = 0;
        }
    };

    class rcu_domain {
        inline thread_local static const std::size_t key = (std::hash<std::thread::id>{}(std::this_thread::get_id()) / 256) % garbage_queue::num_ref_counts;
        inline thread_local static std::uint64_t num_readers = 0;
        inline thread_local static std::atomic<std::size_t>* counter = nullptr;

        std::atomic<std::size_t> generation;
        std::array<garbage_queue, 2> garbage;
        std::mutex mutex;

        struct default_domain_tag_t {};
        explicit rcu_domain(default_domain_tag_t) : generation{}, garbage{}, mutex{} {}

    public:
        rcu_domain() = delete;
        rcu_domain(const rcu_domain&) = delete;
        rcu_domain(rcu_domain&&) = delete;
        rcu_domain& operator=(const rcu_domain&) = delete;
        rcu_domain& operator=(rcu_domain&&) = delete;
        ~rcu_domain() noexcept = default;

        void lock() noexcept {
            if (num_readers == 0)
                counter = &garbage[generation.load(std::memory_order_acquire) % 2].ref_count()[key];
            num_readers++;
            counter->fetch_add(1, std::memory_order_release);
        }
        bool try_lock() noexcept {
            lock();
            return true;
        }

        void unlock() noexcept {
            [[maybe_unused]] auto _ = static_cast<void*>(this); //prevents warning
            num_readers--;
            counter->fetch_sub(1, std::memory_order_release);
        }
        void retire(void* p, deleter_t&& d) noexcept {
            std::lock_guard guard{mutex};
            const auto current_gen = generation.load(std::memory_order_acquire);
            const auto next_gen = (current_gen + 1) % 2;
            auto ptr = garbage_queue::auto_ptr{p, std::move(d)};
            if (!garbage[current_gen % 2].try_push(std::move(ptr))) [[unlikely]] {
                garbage[next_gen].synchronize();
                garbage[next_gen].clear();
                garbage[next_gen].push_unchecked(std::move(ptr));
                generation.store(current_gen + 1, std::memory_order_release);
            }
        }
        void half_sync() noexcept {
            const auto current_gen = generation.load(std::memory_order_acquire);
            const auto target_gen = (current_gen + 1) % 2;
            std::unique_lock guard{mutex};
            if (generation.load(std::memory_order_acquire) < current_gen + 1) [[likely]] {
                garbage[target_gen].synchronize();
                garbage[target_gen].clear();
                generation.store(current_gen + 1, std::memory_order_seq_cst);
            }
        }
        friend void rcu_synchronize(rcu_domain& dom) noexcept;
        friend rcu_domain& rcu_default_domain() noexcept;
    };
    template<class T, class D = std::default_delete<T>>
    void rcu_retire(T* p, D d = D(), rcu_domain& dom = rcu_default_domain()) {
        dom.retire(static_cast<void*>(p), deleter_t{std::type_identity<T>{}, std::move(d)});
    }
    inline rcu_domain& rcu_default_domain() noexcept {
        static rcu_domain domain{rcu_domain::default_domain_tag_t{}};
        return domain;
    }

    inline void rcu_synchronize(rcu_domain& dom = rcu_default_domain()) noexcept {
        dom.half_sync();
        dom.half_sync();
    }

    inline void rcu_barrier(rcu_domain& dom = rcu_default_domain()) noexcept {
        rcu_synchronize(dom);
    }
}
namespace single_ref_count {
    using v1::deleter_t;

    class rcu_domain;
    rcu_domain& rcu_default_domain() noexcept;

    struct garbage_queue {
        static constexpr std::size_t num_ref_counts = 16;
        static constexpr std::size_t capacity = 512;
        using ref_count_t = std::atomic<std::size_t>;
        using padding = std::array<char, 128 - sizeof(ref_count_t)>;
        using auto_ptr = std::pair<void*, deleter_t>;

        //std::array<std::pair<ref_count_t, padding>, num_ref_counts> ref_counts;
        std::atomic<std::uint64_t> ref_count;
        std::array<auto_ptr, capacity> queue;
        std::size_t size;

        garbage_queue() noexcept : ref_count{}, queue{}, size{} {}
        garbage_queue(const garbage_queue&) = delete;
        garbage_queue(garbage_queue&&) = delete;
        garbage_queue& operator=(const garbage_queue&) = delete;
        garbage_queue& operator=(garbage_queue&&) = delete;
        ~garbage_queue() noexcept {
            assert(try_synchronize());
            clear();
        }
        void synchronize() noexcept {
            while (ref_count.load(std::memory_order_relaxed) != 0)
                std::this_thread::yield();
        }
        bool try_synchronize() noexcept {
            return ref_count.load(std::memory_order_acquire) == 0;
        }
        bool try_push(auto_ptr&& ptr) noexcept {
            if (size < capacity) [[likely]] {
                const auto index = size++;
                queue[index] = std::move(ptr);
                return true;
            }
            else {
                return false;
            }
        }
        void push_unchecked(auto_ptr&& ptr) noexcept {
            const auto index = size++;
            queue[index] = std::move(ptr);
        }

        void clear() {
            assert(try_synchronize());
            for (auto&& [p, d] : queue | std::ranges::views::take(size)) {
                d(p);
                p = nullptr;
            }
            size = 0;
        }
    };

    class rcu_domain {
        inline thread_local static const std::size_t key = std::hash<std::thread::id>{}(std::this_thread::get_id()) % garbage_queue::num_ref_counts;
        inline thread_local static std::uint64_t num_readers = 0;
        inline thread_local static std::atomic<std::size_t>* counter = nullptr;

        std::atomic<std::size_t> generation;
        std::array<garbage_queue, 2> garbage;
        std::mutex mutex;

        struct default_domain_tag_t {};
        explicit rcu_domain(default_domain_tag_t) : generation{}, garbage{}, mutex{} {}

    public:
        rcu_domain() = delete;
        rcu_domain(const rcu_domain&) = delete;
        rcu_domain(rcu_domain&&) = delete;
        rcu_domain& operator=(const rcu_domain&) = delete;
        rcu_domain& operator=(rcu_domain&&) = delete;
        ~rcu_domain() noexcept = default;

        void lock() noexcept {
            if (num_readers == 0)
                counter = &garbage[generation.load(std::memory_order_acquire) % 2].ref_count;
            num_readers++;
            counter->fetch_add(1, std::memory_order_release);
        }
        bool try_lock() noexcept {
            lock();
            return true;
        }

        void unlock() noexcept {
            [[maybe_unused]] auto _ = static_cast<void*>(this); //prevents warning
            num_readers--;
            counter->fetch_sub(1, std::memory_order_release);
        }
        void retire(void* p, deleter_t&& d) noexcept {
            std::lock_guard guard{mutex};
            const auto current_gen = generation.load(std::memory_order_acquire);
            const auto next_gen = (current_gen + 1) % 2;
            auto ptr = garbage_queue::auto_ptr{p, std::move(d)};
            if (!garbage[current_gen % 2].try_push(std::move(ptr))) [[unlikely]] {
                garbage[next_gen].synchronize();
                garbage[next_gen].clear();
                garbage[next_gen].push_unchecked(std::move(ptr));
                generation.store(current_gen + 1, std::memory_order_release);
            }
        }
        void half_sync() noexcept {
            const auto current_gen = generation.load(std::memory_order_acquire);
            const auto target_gen = (current_gen + 1) % 2;
            std::unique_lock guard{mutex};
            if (generation.load(std::memory_order_acquire) < current_gen + 1) [[likely]] {
                garbage[target_gen].synchronize();
                garbage[target_gen].clear();
                generation.store(current_gen + 1, std::memory_order_seq_cst);
            }
        }
        friend void rcu_synchronize(rcu_domain& dom) noexcept;
        friend rcu_domain& rcu_default_domain() noexcept;
    };
    template<class T, class D = std::default_delete<T>>
    void rcu_retire(T* p, D d = D(), rcu_domain& dom = rcu_default_domain()) {
        dom.retire(static_cast<void*>(p), deleter_t{std::type_identity<T>{}, std::move(d)});
    }
    inline rcu_domain& rcu_default_domain() noexcept {
        static rcu_domain domain{rcu_domain::default_domain_tag_t{}};
        return domain;
    }

    inline void rcu_synchronize(rcu_domain& dom = rcu_default_domain()) noexcept {
        dom.half_sync();
        dom.half_sync();
    }

    inline void rcu_barrier(rcu_domain& dom = rcu_default_domain()) noexcept {
        rcu_synchronize(dom);
    }
}
namespace false_sharing {
    using v1::deleter_t;

    class rcu_domain;
    rcu_domain& rcu_default_domain() noexcept;

    struct garbage_queue {
        static constexpr std::size_t num_ref_counts = 16;
        static constexpr std::size_t capacity = 512;
        using ref_count_t = std::atomic<std::size_t>;
        using auto_ptr = std::pair<void*, deleter_t>;

        std::array<ref_count_t, num_ref_counts> ref_counts;
        std::array<auto_ptr, capacity> queue;
        std::size_t size;

        auto& ref_count() noexcept {
            return ref_counts;
        }

        garbage_queue() noexcept : ref_counts{}, queue{}, size{} {}
        garbage_queue(const garbage_queue&) = delete;
        garbage_queue(garbage_queue&&) = delete;
        garbage_queue& operator=(const garbage_queue&) = delete;
        garbage_queue& operator=(garbage_queue&&) = delete;
        ~garbage_queue() noexcept {
            assert(try_synchronize());
            clear();
        }
        void synchronize() noexcept {
            for (auto&& i : ref_count()) {
                while (i.load(std::memory_order_relaxed) != 0)
                    std::this_thread::yield();
            }
        }
        bool try_synchronize() noexcept {
            return std::ranges::all_of(ref_count(), [](auto&& count) {
                return count.load(std::memory_order_acquire) == 0;
            });
        }
        bool try_push(auto_ptr&& ptr) noexcept {
            if (size < capacity) [[likely]] {
                const auto index = size++;
                queue[index] = std::move(ptr);
                return true;
            }
            else {
                return false;
            }
        }
        void push_unchecked(auto_ptr&& ptr) noexcept {
            const auto index = size++;
            queue[index] = std::move(ptr);
        }

        void clear() {
            assert(try_synchronize());
            for (auto&& [p, d] : queue | std::ranges::views::take(size)) {
                d(p);
                p = nullptr;
            }
            size = 0;
        }
    };

    class rcu_domain {
        inline thread_local static const std::size_t key = (std::hash<std::thread::id>{}(std::this_thread::get_id()) / 256) % garbage_queue::num_ref_counts;
        inline thread_local static std::uint64_t num_readers = 0;
        inline thread_local static std::atomic<std::size_t>* counter = nullptr;

        std::atomic<std::size_t> generation;
        std::array<garbage_queue, 2> garbage;
        std::mutex mutex;

        struct default_domain_tag_t {};
        explicit rcu_domain(default_domain_tag_t) : generation{}, garbage{}, mutex{} {}

    public:
        rcu_domain() = delete;
        rcu_domain(const rcu_domain&) = delete;
        rcu_domain(rcu_domain&&) = delete;
        rcu_domain& operator=(const rcu_domain&) = delete;
        rcu_domain& operator=(rcu_domain&&) = delete;
        ~rcu_domain() noexcept = default;

        void lock() noexcept {
            if (num_readers == 0)
                counter = &garbage[generation.load(std::memory_order_acquire) % 2].ref_count()[key];
            num_readers++;
            counter->fetch_add(1, std::memory_order_release);
        }
        bool try_lock() noexcept {
            lock();
            return true;
        }

        void unlock() noexcept {
            [[maybe_unused]] auto _ = static_cast<void*>(this); //prevents warning
            num_readers--;
            counter->fetch_sub(1, std::memory_order_release);
        }
        void retire(void* p, deleter_t&& d) noexcept {
            std::lock_guard guard{mutex};
            const auto current_gen = generation.load(std::memory_order_acquire);
            const auto next_gen = (current_gen + 1) % 2;
            auto ptr = garbage_queue::auto_ptr{p, std::move(d)};
            if (!garbage[current_gen % 2].try_push(std::move(ptr))) [[unlikely]] {
                garbage[next_gen].synchronize();
                garbage[next_gen].clear();
                garbage[next_gen].push_unchecked(std::move(ptr));
                generation.store(current_gen + 1, std::memory_order_release);
            }
        }
        void half_sync() noexcept {
            const auto current_gen = generation.load(std::memory_order_acquire);
            const auto target_gen = (current_gen + 1) % 2;
            std::unique_lock guard{mutex};
            if (generation.load(std::memory_order_acquire) < current_gen + 1) [[likely]] {
                garbage[target_gen].synchronize();
                garbage[target_gen].clear();
                generation.store(current_gen + 1, std::memory_order_seq_cst);
            }
        }
        friend void rcu_synchronize(rcu_domain& dom) noexcept;
        friend rcu_domain& rcu_default_domain() noexcept;
    };
    template<class T, class D = std::default_delete<T>>
    void rcu_retire(T* p, D d = D(), rcu_domain& dom = rcu_default_domain()) {
        dom.retire(static_cast<void*>(p), deleter_t{std::type_identity<T>{}, std::move(d)});
    }
    inline rcu_domain& rcu_default_domain() noexcept {
        static rcu_domain domain{rcu_domain::default_domain_tag_t{}};
        return domain;
    }

    inline void rcu_synchronize(rcu_domain& dom = rcu_default_domain()) noexcept {
        dom.half_sync();
        dom.half_sync();
    }

    inline void rcu_barrier(rcu_domain& dom = rcu_default_domain()) noexcept {
        rcu_synchronize(dom);
    }
}
}

#endif //RCU_RCU_H