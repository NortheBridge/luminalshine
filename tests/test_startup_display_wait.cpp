#include "src/startup_display_wait.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using startup::display_wait_callbacks;
using startup::display_wait_result;
using startup::wait_for_physical_display;

namespace {

  // Build a deterministic predicate that returns the values in `script` in
  // order. Each call advances the cursor; after the script is exhausted the
  // last value is returned indefinitely. This lets a test express
  // "display appears on the 3rd poll" without juggling counters.
  auto make_scripted_predicate(std::vector<bool> script) {
    auto state = std::make_shared<std::vector<bool>>(std::move(script));
    auto cursor = std::make_shared<size_t>(0);
    return [state, cursor]() -> bool {
      if (state->empty()) {
        return false;
      }
      const size_t idx = std::min(*cursor, state->size() - 1);
      ++*cursor;
      return (*state)[idx];
    };
  }

  auto make_recording_sleep(std::vector<std::chrono::milliseconds> &sink) {
    return [&sink](std::chrono::milliseconds d) {
      sink.push_back(d);
    };
  }

}  // namespace

TEST(StartupDisplayWait, DisplayReadyOnFirstPoll_ReturnsImmediately) {
  std::vector<std::chrono::milliseconds> sleeps;
  display_wait_callbacks cb {
    make_scripted_predicate({true}),
    [] {
      return false;
    },
    make_recording_sleep(sleeps),
  };
  const auto r = wait_for_physical_display(12, std::chrono::milliseconds {250}, cb);
  EXPECT_TRUE(r.display_ready);
  EXPECT_FALSE(r.aborted);
  EXPECT_EQ(r.attempts, 2);  // initial implicit + 1 poll that returned true
  EXPECT_EQ(sleeps.size(), 1u);
  EXPECT_EQ(sleeps.front(), std::chrono::milliseconds {250});
}

TEST(StartupDisplayWait, DisplayReadyOnThirdPoll_ReturnsRecovered) {
  std::vector<std::chrono::milliseconds> sleeps;
  display_wait_callbacks cb {
    make_scripted_predicate({false, false, true}),
    [] {
      return false;
    },
    make_recording_sleep(sleeps),
  };
  const auto r = wait_for_physical_display(12, std::chrono::milliseconds {250}, cb);
  EXPECT_TRUE(r.display_ready);
  EXPECT_FALSE(r.aborted);
  EXPECT_EQ(r.attempts, 4);  // initial + 3 polls
  EXPECT_EQ(sleeps.size(), 3u);
}

TEST(StartupDisplayWait, DisplayNeverAppears_BudgetExhausted_FallsThrough) {
  std::vector<std::chrono::milliseconds> sleeps;
  display_wait_callbacks cb {
    make_scripted_predicate({false}),
    [] {
      return false;
    },
    make_recording_sleep(sleeps),
  };
  const auto r = wait_for_physical_display(5, std::chrono::milliseconds {250}, cb);
  EXPECT_FALSE(r.display_ready);
  EXPECT_FALSE(r.aborted);
  EXPECT_EQ(r.attempts, 6);  // initial + 5 polls
  EXPECT_EQ(sleeps.size(), 5u);
}

TEST(StartupDisplayWait, AbortFirstIteration_ReturnsAbortedWithoutSleeping) {
  std::vector<std::chrono::milliseconds> sleeps;
  display_wait_callbacks cb {
    make_scripted_predicate({false}),
    [] {
      return true;
    },
    make_recording_sleep(sleeps),
  };
  const auto r = wait_for_physical_display(12, std::chrono::milliseconds {250}, cb);
  EXPECT_FALSE(r.display_ready);
  EXPECT_TRUE(r.aborted);
  EXPECT_EQ(r.attempts, 1);
  EXPECT_TRUE(sleeps.empty()) << "abort must short-circuit before sleeping";
}

TEST(StartupDisplayWait, AbortMidWait_StopsImmediately) {
  std::vector<std::chrono::milliseconds> sleeps;
  // Abort flips to true after two abort checks have returned false.
  int abort_calls = 0;
  display_wait_callbacks cb {
    make_scripted_predicate({false, false, false, false}),
    [&] {
      ++abort_calls;
      return abort_calls > 2;
    },
    make_recording_sleep(sleeps),
  };
  const auto r = wait_for_physical_display(12, std::chrono::milliseconds {250}, cb);
  EXPECT_FALSE(r.display_ready);
  EXPECT_TRUE(r.aborted);
  EXPECT_EQ(r.attempts, 3);  // initial + 2 polls before the third iteration aborted
  EXPECT_EQ(sleeps.size(), 2u);
}

TEST(StartupDisplayWait, ZeroPollBudget_NoSleeps_NoRecovery) {
  std::vector<std::chrono::milliseconds> sleeps;
  display_wait_callbacks cb {
    make_scripted_predicate({true}),  // would succeed, but we never get to ask
    [] {
      return false;
    },
    make_recording_sleep(sleeps),
  };
  const auto r = wait_for_physical_display(0, std::chrono::milliseconds {250}, cb);
  EXPECT_FALSE(r.display_ready);
  EXPECT_FALSE(r.aborted);
  EXPECT_EQ(r.attempts, 1);  // only the implicit initial probe
  EXPECT_TRUE(sleeps.empty());
}

TEST(StartupDisplayWait, NullSleepCallback_FallsBackToRealSleep_StillCompletes) {
  // Use a 1ms poll step and a script that succeeds on the first poll so we
  // exercise the fallback `std::this_thread::sleep_for` branch without
  // making the test wall-clock-slow.
  display_wait_callbacks cb {
    make_scripted_predicate({true}),
    [] {
      return false;
    },
    {}  // no injected sleep — fallback path
  };
  const auto t0 = std::chrono::steady_clock::now();
  const auto r = wait_for_physical_display(12, std::chrono::milliseconds {1}, cb);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  EXPECT_TRUE(r.display_ready);
  EXPECT_EQ(r.attempts, 2);
  EXPECT_GE(elapsed, std::chrono::milliseconds {0});  // sanity: didn't go back in time
}

TEST(StartupDisplayWait, NullAbortCallback_TreatedAsNeverAbort) {
  std::vector<std::chrono::milliseconds> sleeps;
  display_wait_callbacks cb {
    make_scripted_predicate({false, true}),
    {},  // no abort callback at all
    make_recording_sleep(sleeps),
  };
  const auto r = wait_for_physical_display(12, std::chrono::milliseconds {250}, cb);
  EXPECT_TRUE(r.display_ready);
  EXPECT_FALSE(r.aborted);
  EXPECT_EQ(r.attempts, 3);
}
