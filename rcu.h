#ifndef RCU_RCU_H
#define RCU_RCU_H
#include <algorithm>
#include <atomic>
#include <cassert>
#include <forward_list>
#include <functional>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>

namespace rcu {
    struct deleter_t {
        template<class D>
        static constexpr bool SBO = sizeof(D) <= 7 && alignof(D) <= alignof(void*);

        struct vtable_t {
            std::reference_wrapper<void(void**, void*)> invoke_ref;
            std::reference_wrapper<void(void*)> dealloc_ref;
        };
        template<class T, class D>
        struct vtable {
            static void invoke(void** deleter, void* p) {
                if constexpr (!std::is_same_v<D, void>) {
                    if constexpr(SBO<D>) {
                        std::invoke(*reinterpret_cast<D*>(deleter), static_cast<T*>(p));
                    }
                    else {
                        std::invoke(*static_cast<D*>(*deleter), static_cast<T*>(p));
                    }
                }
            }
            static void dealloc(void* d) {
                if constexpr (!std::is_same_v<D, void>) {
                    if constexpr(SBO<D>) {
                        static_cast<D*>(d)->~D();
                    }
                    else {
                        delete static_cast<D*>(d);
                    }
                }
            }
            static constexpr vtable_t value = {invoke, dealloc};
        };

        template<class T, class D>
        static constexpr vtable_t vtable_v = vtable<T, D>::value;

        static constexpr vtable_t empty_vtable = vtable<void, void>::value;

        void* deleter_ptr;
        std::reference_wrapper<const vtable_t> vtable_ref;

        template<class T, class D>
        explicit deleter_t(std::type_identity<T>, D d = D()) :
            deleter_ptr{}, vtable_ref{vtable_v<T, D>} {
            if constexpr(SBO<D>) {
                new (deleter_ptr) D{std::forward<D>(d)};
            }
            else {
                deleter_ptr = new D{std::forward<D>(d)};
            }
        }
        deleter_t() : deleter_ptr{}, vtable_ref{empty_vtable} {}
        deleter_t(const deleter_t&) = delete;
        deleter_t(deleter_t&& rhs) noexcept :
            deleter_ptr{std::exchange(rhs.deleter_ptr, nullptr)},
            vtable_ref{rhs.vtable_ref} {}
        deleter_t& operator=(const deleter_t&) = delete;
        deleter_t& operator=(deleter_t&& rhs) noexcept {
            std::swap(deleter_ptr, rhs.deleter_ptr);
            std::swap(vtable_ref, rhs.vtable_ref);
            return *this;
        };
        ~deleter_t() noexcept {
            std::invoke(vtable_ref.get().dealloc_ref, deleter_ptr);
        }
        void operator()(void* p) {
            std::invoke(vtable_ref.get().invoke_ref, &deleter_ptr, p);
        }
        void clear() noexcept {
            std::invoke(vtable_ref.get().dealloc_ref, deleter_ptr);
            deleter_ptr = nullptr;
            vtable_ref = empty_vtable;
        }
        [[nodiscard]] bool is_empty() const noexcept {
            return &vtable_ref.get() == &empty_vtable;
        }
    };

    class rcu_domain;
    rcu_domain& rcu_default_domain() noexcept;

    class rcu_domain {
        static constexpr std::size_t num_ref_counts = 4;
        //static constexpr std::size_t gen_size = 16 * num_ref_counts;
        static constexpr std::size_t max_gens = 4;

        thread_local static const std::size_t key;
        thread_local static std::uint64_t num_readers;
        thread_local static std::atomic<std::size_t>* counter;

        struct gen_t {
            using ref_count_t = std::atomic<std::size_t>;
            using auto_ptr = std::pair<void*, deleter_t>;
            static constexpr std::size_t num_ptrs_per_group = 64 / sizeof(auto_ptr);
            using group = std::pair<ref_count_t, std::array<auto_ptr, num_ptrs_per_group>>;
            static constexpr std::size_t ptr_capacity = num_ptrs_per_group * num_ref_counts;
            using overflow_group = std::array<auto_ptr, ptr_capacity>;

            std::array<group, num_ref_counts> garbage_queue;
            std::forward_list<overflow_group> overflow;
            std::size_t first_overflow_group_size;

            auto ref_count() noexcept {
                return garbage_queue | std::ranges::views::keys;
            }
            auto garbage() noexcept {
                return garbage_queue | std::ranges::views::values | std::ranges::views::join;
            }

            std::size_t size{};

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
                if (size + 1 >= ptr_capacity) {
                    if (overflow.empty() || first_overflow_group_size + 1 >= ptr_capacity) {
                        overflow.emplace_front();
                        first_overflow_group_size = 0;
                    }
                    overflow.front()[first_overflow_group_size] = {ptr, std::move(d)};
                    first_overflow_group_size++;
                }
                else {
                    garbage_queue[size / num_ptrs_per_group].second[size % num_ptrs_per_group] = {ptr, std::move(d)};
                    size++;
                }
            }
            void clear() {
                assert(try_synchronize());
                for (auto&& [p, d] : garbage() | std::ranges::views::take(size)) {
                    d(p);
                    p = nullptr;
                    d.clear();
                }
                for (auto &&[p, d]: overflow | std::ranges::views::join) {
                    d(p);
                    p = nullptr;
                    d.clear();
                }
                overflow.clear();
                size = 0;
                first_overflow_group_size = 0;
            }
            [[nodiscard]] bool is_full() const {
                return size + 1 >= ptr_capacity && first_overflow_group_size >= ptr_capacity;
            }
        };

        std::atomic<std::size_t> generation;
        std::array<gen_t, max_gens> garbage;

        struct default_domain_tag_t {};
        explicit rcu_domain(default_domain_tag_t) : generation{}, garbage{} {}

        auto garbage_queue_view() noexcept {
            return garbage | std::ranges::views::transform([](gen_t& gen) {
                return gen.garbage();
            });
        }
        auto ref_count_view() noexcept {
            return garbage | std::ranges::views::transform([](gen_t& gen) {
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
            num_readers--;
            counter->fetch_sub(1, std::memory_order_release);
        }

        void retire(void* p, deleter_t&& d) {
            auto current_gen = generation.load(std::memory_order_acquire);
            if (garbage[current_gen % max_gens].is_full() && garbage[(current_gen + 1) % max_gens].try_synchronize()) {
                current_gen++;
                generation.store(current_gen, std::memory_order_release);
                garbage[current_gen % max_gens].clear();
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
        for (auto&& i : dom.garbage) {
            i.synchronize();
            i.clear();
        }
    }

    inline void rcu_barrier(rcu_domain& dom = rcu_default_domain()) noexcept {
        rcu_synchronize(dom);
    }

}

#endif //RCU_RCU_H