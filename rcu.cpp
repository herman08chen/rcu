#include <atomic>
#include <functional>
#include <mutex>


namespace rcu {
  template<class T>
  class owning_ptr{
  public:
    struct block {
      T data;
      std::atomic<std::size_t> counter{0};
    };
    class updater {
      std::lock_guard<std::mutex> guard;
      block* new_block;
      owning_ptr& ptr;
    public:
      explicit updater(owning_ptr& ptr) :
        guard(ptr.update_mutex),
        new_block(new block{ptr.copy()}),
        ptr(ptr) {}
      ~updater() {
        auto* old_data = ptr._data.exchange(new_block/*, std::memory_order_release*/);
        while((old_data->counter).load(/*std::memory_order_relaxed*/)) {}
        delete old_data;
      }
      operator T&() { return new_block->data; }
    };

    class block_reader {
      block* const ptr;
    public:
      explicit block_reader(block* ptr) : ptr(ptr) { (ptr->counter).fetch_add(1, std::memory_order_acquire); }
      ~block_reader() { (ptr->counter).fetch_sub(1, std::memory_order_acquire); }
      const T& operator*() const {
        return ptr->data;
      }
      const T& get() const {
        return ptr->data;
      }
      const T& operator->() const { return &get(); }
    };

    explicit owning_ptr(const T& data) : _data(new block{data}) {}
    explicit owning_ptr(T&& data) : _data(new block{std::move(data)}) {}
    ~owning_ptr() { delete _data.load(); }

    block_reader read() const { return block_reader{_data.load(/*std::memory_order_acquire*/)}; }
    T copy() const { return _data.load(/*std::memory_order_acquire*/)->data; }

    void update(auto&& data)
    { update( new block{std::forward<decltype(data)>(data)} ); }
    void update(block* data) {
      std::lock_guard<std::mutex> guard(update_mutex);
      auto* old_data = _data.exchange(data/*, std::memory_order_release*/);
      while((old_data->counter).load(/*std::memory_order_relaxed*/)) {}
      delete old_data;
    }

    [[nodiscard]] updater raii_updater() { return updater{*this}; }

    auto cref() const { return std::cref(*this); }
    auto ref() const { return std::cref(*this); }
    auto ref() { return std::ref(*this); }
  
  private:
    std::atomic<block*> _data;
    std::mutex update_mutex;
  };
};
