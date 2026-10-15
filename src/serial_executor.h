/**
 * @file src/serial_executor.h
 * @brief Single-thread FIFO executor.
 */
#pragma once

// standard includes
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace util {

  /**
   * @brief Runs posted tasks one at a time, in posting order, on one dedicated thread.
   * @details nvhttp uses it to keep the slow /launch, /resume and /cancel handlers off the
   *          HTTPS server's io thread: they stay serialised with each other, exactly as when
   *          they ran inline on that thread, while every other request keeps being answered.
   * @note Tasks must not throw: an exception leaving a task terminates the process.
   *       The destructor stops the executor.
   */
  class serial_executor_t {
  public:
    using task_t = std::function<void()>;

    /**
     * @brief Start the worker thread.
     * @param on_thread_start Optional callback run on the worker thread before it takes tasks.
     */
    explicit serial_executor_t(std::function<void()> on_thread_start = {}):
        worker {[this, on_thread_start = std::move(on_thread_start)]() {
          if (on_thread_start) {
            on_thread_start();
          }
          run();
        }} {}

    serial_executor_t(const serial_executor_t &) = delete;
    serial_executor_t &operator=(const serial_executor_t &) = delete;

    ~serial_executor_t() {
      stop();
    }

    /**
     * @brief Queue a task behind the ones already posted.
     * @return `false`, without queuing, once stop() has begun.
     */
    bool post(task_t task) {
      {
        std::lock_guard lock {mutex};
        if (stopped) {
          return false;
        }
        queue.push_back(std::move(task));
      }
      wake.notify_one();
      return true;
    }

    /**
     * @brief Refuse new tasks, drop the queued ones without running them, and wait for the
     *        task that is running (if any) to finish.
     * @details Dropped tasks are destroyed on the calling thread, outside the queue lock.
     *          Idempotent, but must be called from one thread at a time and never from a task.
     */
    void stop() {
      std::deque<task_t> dropped;
      {
        std::lock_guard lock {mutex};
        stopped = true;
        dropped.swap(queue);
      }
      wake.notify_all();
      dropped.clear();
      if (worker.joinable()) {
        worker.join();
      }
    }

  private:
    void run() {
      std::unique_lock lock {mutex};
      while (true) {
        wake.wait(lock, [this]() {
          return stopped || !queue.empty();
        });
        if (stopped) {
          return;
        }
        auto task = std::move(queue.front());
        queue.pop_front();
        lock.unlock();
        task();
        task = nullptr;
        lock.lock();
      }
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<task_t> queue;
    bool stopped {false};
    // Last: the thread starts in the constructor and uses the members above.
    std::thread worker;
  };

}  // namespace util
