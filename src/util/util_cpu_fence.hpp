#pragma once
#include <atomic>
#include <cstdint>

namespace mullion {

// std::atomic::wait while `value` holds `old`, without the clock read when it no longer does: libc++'s wait reads
// high_resolution_clock before it first looks at the value (__libcpp_thread_poll_with_backoff), and that clock is a
// system call in Wine on macOS (QueryPerformanceCounter: hundreds of ns, and the dispatcher's register saves). the
// encoding thread waits so for each pipeline it sets, which a pipeline compiled long ago never needs
template <typename T, typename U>
inline void
wait_while(const std::atomic<T> &value, U old, std::memory_order order = std::memory_order_seq_cst) {
  if (value.load(order) == T(old))
    value.wait(T(old), order);
}

class CpuFence {
public:
  void wait(uint64_t value) {
    auto current = value_.load(std::memory_order_acquire);
    while (current < value) {
      value_.wait(current);
      current = value_.load(std::memory_order_acquire);
    }
  }

  void signal(uint64_t value) {
    auto current = value_.load(std::memory_order_relaxed);
    do {
      if (value <= current)
        return;
    } while (!value_.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed));
    value_.notify_all();
  }

  uint64_t signaledValue() {
    return value_.load(std::memory_order_acquire);
  }

  CpuFence(): value_(0) {}
  CpuFence(uint64_t initial_value): value_(initial_value) {}

private:
  std::atomic<uint64_t> value_;
};
} // namespace mullion