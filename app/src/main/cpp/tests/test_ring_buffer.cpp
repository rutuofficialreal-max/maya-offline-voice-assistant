#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "ring_buffer.h"
using namespace maya;
int main() {
  SpscRing<int> ring(1000);                      // rounds up to 1024
  const int total = 3000000;
  std::atomic<bool> bad{false};
  std::thread prod([&] {
    std::vector<int> buf(97); int next = 0;
    while (next < total) {
      int n = std::min<int>(97, total - next);
      for (int i = 0; i < n; ++i) buf[i] = next + i;
      size_t w = ring.write(buf.data(), n);
      next += (int)w;
      // partial writes: the rest is retried with the right offset
      if (w < (size_t)n) { next -= 0; /* values were generated from `next`, so just loop */ }
    }
  });
  std::thread cons([&] {
    std::vector<int> buf(61); int expect = 0;
    while (expect < total) {
      size_t r = ring.read(buf.data(), 61);
      for (size_t i = 0; i < r; ++i) if (buf[i] != expect + (int)i) { bad = true; }
      expect += (int)r;
    }
  });
  prod.join(); cons.join();
  // discardAll
  int x[4] = {1,2,3,4}; ring.write(x, 4); ring.discardAll();
  printf("capacity=%zu size_after_discard=%zu corrupted=%s\n", ring.capacity(), ring.size(), bad ? "YES" : "no");
  return (bad || ring.size() != 0) ? 1 : 0;
}
