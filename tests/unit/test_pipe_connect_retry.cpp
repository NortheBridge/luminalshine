/**
 * @file tests/unit/test_pipe_connect_retry.cpp
 * @brief Unit tests for the named-pipe client connect retry policy.
 *
 * Covers the decision logic behind NamedPipeFactory::create_client_pipe: a client that races the
 * display helper's startup must keep polling for the pipe up to its time budget, stop as soon as
 * the caller aborts the wait (helper process exited, shutdown), and never overshoot the budget by
 * more than one attempt. The policy is pure (no Win32), so these tests run on every platform.
 */
#include "../tests_common.h"
#include "src/platform/windows/ipc/pipe_connect_retry.h"

#include <chrono>
#include <optional>

using namespace std::chrono_literals;
using platf::ipc::ConnectAction;
using platf::ipc::ConnectAttempt;
using platf::ipc::ConnectRetryPolicy;
using platf::ipc::ConnectStep;
using platf::ipc::GiveUpReason;
using platf::ipc::next_connect_step;

namespace {
  constexpr ConnectRetryPolicy kDefaultPolicy {};

  /**
   * @brief Drive the policy against a scripted server with a fake clock.
   *
   * The server's pipe appears at `pipe_ready_at` (never, if nullopt) and the server process exits at
   * `exit_at` (never, if nullopt). Each attempt advances the clock by the wait the policy asked for.
   */
  struct SimulatedConnect {
    std::chrono::milliseconds elapsed {0};
    int attempts = 0;
    ConnectStep last {};

    static SimulatedConnect run(
      const ConnectRetryPolicy &policy,
      std::optional<std::chrono::milliseconds> pipe_ready_at,
      std::optional<std::chrono::milliseconds> exit_at,
      bool busy_until_ready = false
    ) {
      SimulatedConnect sim;
      for (int guard = 0; guard < 100000; ++guard) {
        ++sim.attempts;
        const bool ready = pipe_ready_at && sim.elapsed >= *pipe_ready_at;
        const bool exited = exit_at && sim.elapsed >= *exit_at;
        const ConnectAttempt attempt = ready            ? ConnectAttempt::connected :
                                       busy_until_ready ? ConnectAttempt::server_busy :
                                                          ConnectAttempt::server_not_found;
        sim.last = next_connect_step(policy, sim.elapsed, attempt, exited);
        if (sim.last.action == ConnectAction::done || sim.last.action == ConnectAction::give_up) {
          return sim;
        }
        sim.elapsed += sim.last.wait;
      }
      ADD_FAILURE() << "simulation did not terminate";
      return sim;
    }
  };
}  // namespace

TEST(PipeConnectRetry, SuccessfulAttemptIsDoneRegardlessOfLiveness) {
  const auto step = next_connect_step(kDefaultPolicy, 0ms, ConnectAttempt::connected, /*abort_requested=*/true);
  EXPECT_EQ(step.action, ConnectAction::done);
  EXPECT_EQ(step.wait, 0ms);
  EXPECT_EQ(step.reason, GiveUpReason::none);
}

TEST(PipeConnectRetry, FatalErrorGivesUpImmediately) {
  const auto step = next_connect_step(kDefaultPolicy, 0ms, ConnectAttempt::fatal, false);
  EXPECT_EQ(step.action, ConnectAction::give_up);
  EXPECT_EQ(step.reason, GiveUpReason::fatal_error);
}

TEST(PipeConnectRetry, NotFoundPollsWithinBudget) {
  const auto step = next_connect_step(kDefaultPolicy, 0ms, ConnectAttempt::server_not_found, false);
  EXPECT_EQ(step.action, ConnectAction::poll_not_found);
  EXPECT_EQ(step.wait, kDefaultPolicy.not_found_poll);
  EXPECT_EQ(step.reason, GiveUpReason::none);
}

TEST(PipeConnectRetry, BusyWaitsOnNamedPipeWithinBudget) {
  const auto step = next_connect_step(kDefaultPolicy, 0ms, ConnectAttempt::server_busy, false);
  EXPECT_EQ(step.action, ConnectAction::wait_busy);
  EXPECT_EQ(step.wait, kDefaultPolicy.busy_wait);
}

TEST(PipeConnectRetry, WaitsAreClampedToRemainingBudget) {
  const auto not_found = next_connect_step(kDefaultPolicy, 480ms, ConnectAttempt::server_not_found, false);
  EXPECT_EQ(not_found.action, ConnectAction::poll_not_found);
  EXPECT_EQ(not_found.wait, 20ms);

  const auto busy = next_connect_step(kDefaultPolicy, 400ms, ConnectAttempt::server_busy, false);
  EXPECT_EQ(busy.action, ConnectAction::wait_busy);
  EXPECT_EQ(busy.wait, 100ms);
}

TEST(PipeConnectRetry, DeadlineGivesUp) {
  const auto at_deadline = next_connect_step(kDefaultPolicy, 500ms, ConnectAttempt::server_not_found, false);
  EXPECT_EQ(at_deadline.action, ConnectAction::give_up);
  EXPECT_EQ(at_deadline.reason, GiveUpReason::deadline);

  const auto past_deadline = next_connect_step(kDefaultPolicy, 900ms, ConnectAttempt::server_busy, false);
  EXPECT_EQ(past_deadline.action, ConnectAction::give_up);
  EXPECT_EQ(past_deadline.reason, GiveUpReason::deadline);
}

TEST(PipeConnectRetry, ZeroBudgetMeansSingleAttempt) {
  ConnectRetryPolicy single_shot;
  single_shot.max_wait = 0ms;
  const auto step = next_connect_step(single_shot, 0ms, ConnectAttempt::server_not_found, false);
  EXPECT_EQ(step.action, ConnectAction::give_up);
  EXPECT_EQ(step.reason, GiveUpReason::deadline);
}

TEST(PipeConnectRetry, AbortGivesUpBeforeDeadline) {
  ConnectRetryPolicy policy;
  policy.max_wait = 5000ms;

  const auto not_found = next_connect_step(policy, 100ms, ConnectAttempt::server_not_found, true);
  EXPECT_EQ(not_found.action, ConnectAction::give_up);
  EXPECT_EQ(not_found.reason, GiveUpReason::aborted);

  // A busy pipe with our server gone belongs to something else; do not wait on it.
  const auto busy = next_connect_step(policy, 100ms, ConnectAttempt::server_busy, true);
  EXPECT_EQ(busy.action, ConnectAction::give_up);
  EXPECT_EQ(busy.reason, GiveUpReason::aborted);
}

TEST(PipeConnectRetry, AbortOutranksDeadlineReason) {
  const auto step = next_connect_step(kDefaultPolicy, 5000ms, ConnectAttempt::server_not_found, true);
  EXPECT_EQ(step.action, ConnectAction::give_up);
  EXPECT_EQ(step.reason, GiveUpReason::aborted);
}

TEST(PipeConnectRetry, WaitsAreNeverZeroWhileRetrying) {
  ConnectRetryPolicy policy;
  policy.not_found_poll = 0ms;
  policy.busy_wait = 0ms;
  // A zero WaitNamedPipe timeout means "use the server default" on Windows, and a zero sleep would
  // spin; the policy must hand back at least 1 ms.
  EXPECT_EQ(next_connect_step(policy, 0ms, ConnectAttempt::server_not_found, false).wait, 1ms);
  EXPECT_EQ(next_connect_step(policy, 0ms, ConnectAttempt::server_busy, false).wait, 1ms);
}

// The scenario from the field: the helper logged "successfully started" but under load took ~0.9 s
// to create its pipe. The historical 500 ms cap gave up; a liveness-bounded 5 s budget must wait it out.
TEST(PipeConnectRetry, SlowServerStartIsWaitedOutWithLargeBudget) {
  ConnectRetryPolicy policy;
  policy.max_wait = 5000ms;
  const auto sim = SimulatedConnect::run(policy, 900ms, std::nullopt);
  EXPECT_EQ(sim.last.action, ConnectAction::done);
  EXPECT_GE(sim.elapsed, 900ms);
  EXPECT_LT(sim.elapsed, 900ms + policy.not_found_poll);
}

TEST(PipeConnectRetry, HistoricalDefaultStillCapsAtHalfASecond) {
  const auto sim = SimulatedConnect::run(kDefaultPolicy, 900ms, std::nullopt);
  EXPECT_EQ(sim.last.action, ConnectAction::give_up);
  EXPECT_EQ(sim.last.reason, GiveUpReason::deadline);
  EXPECT_EQ(sim.elapsed, 500ms);
}

TEST(PipeConnectRetry, CrashedServerIsReportedPromptly) {
  ConnectRetryPolicy policy;
  policy.max_wait = 5000ms;
  const auto sim = SimulatedConnect::run(policy, std::nullopt, 300ms);
  EXPECT_EQ(sim.last.action, ConnectAction::give_up);
  EXPECT_EQ(sim.last.reason, GiveUpReason::aborted);
  EXPECT_GE(sim.elapsed, 300ms);
  EXPECT_LT(sim.elapsed, 300ms + policy.not_found_poll);
}

TEST(PipeConnectRetry, NeverOvershootsBudgetByMoreThanOneAttempt) {
  ConnectRetryPolicy policy;
  policy.max_wait = 1234ms;
  policy.not_found_poll = 50ms;
  const auto sim = SimulatedConnect::run(policy, std::nullopt, std::nullopt);
  EXPECT_EQ(sim.last.action, ConnectAction::give_up);
  EXPECT_EQ(sim.last.reason, GiveUpReason::deadline);
  EXPECT_EQ(sim.elapsed, policy.max_wait);  // the final wait is clamped, so the deadline is hit exactly
}

TEST(PipeConnectRetry, BusyServerIsWaitedOnUntilFree) {
  ConnectRetryPolicy policy;
  policy.max_wait = 5000ms;
  const auto sim = SimulatedConnect::run(policy, 600ms, std::nullopt, /*busy_until_ready=*/true);
  EXPECT_EQ(sim.last.action, ConnectAction::done);
  EXPECT_GE(sim.elapsed, 600ms);
  EXPECT_LE(sim.elapsed, 600ms + policy.busy_wait);
}
