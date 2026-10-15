/**
 * @file src/serial_executor.h
 * @brief Single-thread FIFO executor.
 */
#pragma once

// standard includes
#include <functional>
#include <utility>

namespace util {

  /**
   * @brief Placeholder: runs every task inline on the calling thread.
   * @note Replaced by the real worker-thread implementation in the next commit.
   */
  class serial_executor_t {
  public:
    using task_t = std::function<void()>;

    explicit serial_executor_t(std::function<void()> on_thread_start = {}) {
      if (on_thread_start) {
        on_thread_start();
      }
    }

    bool post(task_t task) {
      task();
      return true;
    }

    void stop() {}
  };

}  // namespace util
