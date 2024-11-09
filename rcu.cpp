#include <atomic>
#include <functional>
#include <type_traits>

namespace rcu {
  template<class T>
  class data_ptr{
  public:
    struct block {
      const T data;
      std::atomic<std::size_t> counter;
      block(auto&& i) : data(std::forward<decltype(i)>(i)), counter(0) {}
      ~block() = default;
    };
    class block_reader {
      block* const ptr;
    public:
      block_reader(block* ptr) : ptr(ptr) { (ptr->counter).fetch_add(1, std::memory_order_acquire); }
      ~block_reader() { (ptr->counter).fetch_sub(1, std::memory_order_acquire); }
      const T& operator*() const {
        return ptr->data;
      }
      const T& get() const {
        return ptr->data;
      }
    };

    data_ptr(T& data)  : _data(new block{data}) {}
    data_ptr(T&& data) : _data(new block{std::move(data)}) {}
    ~data_ptr() {
      delete _data.load();
    } 

    block_reader read() const {
      return block_reader(_data.load(std::memory_order_acquire));
    }
    T copy() const {
      return (_data.load(std::memory_order_acquire))->data;
    }
    void update(auto&& data) 
    requires std::is_same_v<std::remove_cvref_t<T>, std::remove_cvref_t<decltype(data)>>
    {
      std::lock_guard<std::mutex> guard(update_mutex);
      auto* old_data = _data.exchange(new block(std::forward<decltype(data)>(data)), std::memory_order_release);
      while((old_data->counter).load(std::memory_order_relaxed)) {}
      //std::println("deleting old data...");
      delete old_data;
    }
    auto cref() const { return std::cref(*this); }
    auto ref() const { return std::cref(*this); }
    auto ref() { return std::ref(*this); }
  
  private:
    std::atomic<block*> _data;
    std::mutex update_mutex;
  };
};
