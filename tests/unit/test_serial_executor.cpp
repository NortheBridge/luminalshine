/**
 * @file tests/unit/test_serial_executor.cpp
 * @brief Tests for util::serial_executor_t, the single-thread FIFO executor
 *        that keeps long /launch and /resume work off the HTTPS io thread.
 */

#include "src/serial_executor.h"

#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

  // Every wait is bounded so a regression fails the test instead of hanging the run.
  constexpr auto kWait = 5s;

  TEST(SerialExecutor, RunsTasksInFifoOrderOnOneWorkerThread) {
    util::serial_executor_t executor;

    constexpr int kTasks = 50;
    std::mutex mutex;
    std::vector<int> order;
    std::vector<std::thread::id> threads;
    std::promise<void> all_done;

    for (int i = 0; i < kTasks; ++i) {
      ASSERT_TRUE(executor.post([&, i]() {
        {
          std::lock_guard lock {mutex};
          order.push_back(i);
          threads.push_back(std::this_thread::get_id());
        }
        if (i == kTasks - 1) {
          all_done.set_value();
        }
      }));
    }

    ASSERT_EQ(all_done.get_future().wait_for(kWait), std::future_status::ready);
    executor.stop();

    ASSERT_EQ(order.size(), static_cast<std::size_t>(kTasks));
    for (int i = 0; i < kTasks; ++i) {
      EXPECT_EQ(order[i], i);
    }
    for (const auto &id : threads) {
      EXPECT_EQ(id, threads.front());
    }
    EXPECT_NE(threads.front(), std::this_thread::get_id());
  }

  TEST(SerialExecutor, PostReturnsBeforeALongTaskFinishes) {
    util::serial_executor_t executor;

    std::promise<void> running;
    std::promise<void> release;
    auto release_future = release.get_future().share();
    std::atomic<bool> finished {false};

    const auto posted_at = std::chrono::steady_clock::now();
    ASSERT_TRUE(executor.post([&]() {
      running.set_value();
      release_future.wait_for(kWait);
      finished.store(true);
    }));
    const auto post_took = std::chrono::steady_clock::now() - posted_at;

    // The task is running and blocked, yet post() has already returned.
    ASSERT_EQ(running.get_future().wait_for(kWait), std::future_status::ready);
    EXPECT_FALSE(finished.load());

    release.set_value();
    executor.stop();

    EXPECT_LT(post_took, 2s);
    EXPECT_TRUE(finished.load());
  }

  TEST(SerialExecutor, RunsTheStartCallbackOnTheWorkerThread) {
    std::promise<std::thread::id> started_on;
    util::serial_executor_t executor {[&]() {
      started_on.set_value(std::this_thread::get_id());
    }};

    auto future = started_on.get_future();
    ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
    EXPECT_NE(future.get(), std::this_thread::get_id());
  }

  TEST(SerialExecutor, StopDiscardsQueuedTasksAndWaitsForTheRunningOne) {
    util::serial_executor_t executor;

    std::promise<void> running;
    std::promise<void> release;
    auto release_future = release.get_future().share();
    std::atomic<bool> first_finished {false};
    std::atomic<bool> queued_ran {false};

    ASSERT_TRUE(executor.post([&]() {
      running.set_value();
      release_future.wait_for(kWait);
      first_finished.store(true);
    }));
    ASSERT_EQ(running.get_future().wait_for(kWait), std::future_status::ready);

    // Queued behind the running task: must be dropped by stop(), never run.
    ASSERT_TRUE(executor.post([&]() {
      queued_ran.store(true);
    }));

    std::thread stopper {[&]() {
      executor.stop();
    }};

    // stop() refuses new work as soon as it begins, even though it is still
    // waiting for the running task.
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    bool rejected = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!executor.post([]() {
          })) {
        rejected = true;
        break;
      }
      std::this_thread::sleep_for(5ms);
    }

    release.set_value();
    stopper.join();

    EXPECT_TRUE(rejected);
    EXPECT_TRUE(first_finished.load());
    EXPECT_FALSE(queued_ran.load());
  }

  TEST(SerialExecutor, PostAfterStopIsRejected) {
    util::serial_executor_t executor;
    executor.stop();

    std::atomic<bool> ran {false};
    EXPECT_FALSE(executor.post([&]() {
      ran.store(true);
    }));
    EXPECT_FALSE(ran.load());

    // Stopping twice is harmless.
    executor.stop();
  }

}  // namespace
