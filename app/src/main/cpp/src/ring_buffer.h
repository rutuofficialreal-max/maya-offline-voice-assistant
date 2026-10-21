// Lock-free single-producer / single-consumer ring buffer (power-of-two capacity).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>

namespace maya {

template <typename T>
class SpscRing {
 public:
  explicit SpscRing(size_t minCapacity) {
    cap_ = 1;
    while (cap_ < minCapacity) cap_ <<= 1;
    mask_ = cap_ - 1;
    buf_.reset(new T[cap_]());
  }

  size_t capacity() const { return cap_; }

  // Safe from any thread (read order matters: r first, then w, so w >= r holds).
  size_t size() const {
    const size_t r = r_.load(std::memory_order_acquire);
    const size_t w = w_.load(std::memory_order_acquire);
    return w - r;
  }

  // Producer thread only. Returns the number of elements actually written.
  size_t write(const T* src, size_t n) {
    const size_t w = w_.load(std::memory_order_relaxed);
    const size_t r = r_.load(std::memory_order_acquire);
    const size_t freeN = cap_ - (w - r);
    if (n > freeN) n = freeN;
    const size_t pos = w & mask_;
    const size_t first = std::min(n, cap_ - pos);
    std::memcpy(&buf_[pos], src, first * sizeof(T));
    if (n > first) std::memcpy(&buf_[0], src + first, (n - first) * sizeof(T));
    w_.store(w + n, std::memory_order_release);
    return n;
  }

  // Consumer thread only. Returns the number of elements actually read.
  size_t read(T* dst, size_t n) {
    const size_t r = r_.load(std::memory_order_relaxed);
    const size_t w = w_.load(std::memory_order_acquire);
    const size_t avail = w - r;
    if (n > avail) n = avail;
    const size_t pos = r & mask_;
    const size_t first = std::min(n, cap_ - pos);
    std::memcpy(dst, &buf_[pos], first * sizeof(T));
    if (n > first) std::memcpy(dst + first, &buf_[0], (n - first) * sizeof(T));
    r_.store(r + n, std::memory_order_release);
    return n;
  }

  // Consumer thread only: drop everything currently buffered.
  void discardAll() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  size_t cap_ = 0;
  size_t mask_ = 0;
  std::unique_ptr<T[]> buf_;
  std::atomic<size_t> w_{0};
  std::atomic<size_t> r_{0};
};

}  // namespace maya
