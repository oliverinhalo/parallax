#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace parallax {

/// Above this a phone spends more time throttling than computing.
constexpr int kMaxParallelThreads = 6;

/**
 * Run `body(begin, end)` over `[0, count)` split into contiguous blocks, one per thread.
 *
 * The engine was entirely single-threaded, which was fine until the first real photographs went
 * through it: depth estimation was 97% of the run at forty seconds a frame, on four cores, with
 * three of them idle. A plane sweep is the ideal case for this -- every pixel's answer depends
 * only on the images, so there is no shared state to guard and no ordering to preserve.
 *
 * Blocks rather than a work queue because the cost per pixel is near-uniform: a queue's
 * synchronisation would be most of the saving on a short row.
 *
 * `threads` of zero asks the hardware. The cap is not about this machine but about the phone:
 * reconstruction runs as a background job while the user does something else, and saturating
 * every core heats the device into the thermal throttling the capture stage already fights.
 */
inline void parallelFor(size_t count, int threads,
                        const std::function<void(size_t, size_t)>& body) {
  if (count == 0) return;
  int wanted = threads;
  if (wanted <= 0) {
    const unsigned hardware = std::thread::hardware_concurrency();
    wanted = hardware == 0 ? 1 : static_cast<int>(hardware);
  }
  wanted = std::min<int>(wanted, kMaxParallelThreads);
  // One block per thread, and never more threads than there is work for.
  wanted = std::max(1, std::min<int>(wanted, static_cast<int>(count)));

  if (wanted == 1) {
    body(0, count);
    return;
  }

  const size_t block = (count + wanted - 1) / wanted;
  std::vector<std::thread> workers;
  workers.reserve(wanted - 1);
  for (int i = 1; i < wanted; ++i) {
    const size_t begin = std::min(count, block * i);
    const size_t end = std::min(count, begin + block);
    if (begin >= end) break;
    workers.emplace_back([&body, begin, end] { body(begin, end); });
  }
  // The calling thread takes the first block rather than waiting on all of them.
  body(0, std::min(count, block));
  for (std::thread& worker : workers) worker.join();
}

}  // namespace parallax
