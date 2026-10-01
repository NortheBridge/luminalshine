/**
 * @file src/session_lifecycle_coordinator.h
 * @brief Serializes display preparation against last-session cleanup.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace stream::lifecycle {

  /**
   * @brief Decide whether display preparation may treat capture as idle.
   *
   * The RTSP value is the logical RUNNING count, not the container size. The
   * latter requires reaping STOPPING sessions and may synchronously enter
   * session::join(), which must never happen while a launch lease is held.
   */
  constexpr bool capture_is_logically_idle(
    unsigned int active_rtsp_sessions,
    bool webrtc_active
  ) noexcept {
    return active_rtsp_sessions == 0 && !webrtc_active;
  }

  /**
   * @brief Revalidate last-session cleanup after exclusive ownership is won.
   *
   * The atomic decrement that observed zero and the coordinator acquisition
   * are separate operations. A successor may finish session::start between
   * them, so cleanup must check both counters again under its lease.
   */
  constexpr bool last_session_cleanup_still_valid(
    unsigned int running_rtsp_sessions,
    unsigned int active_rtsp_sessions
  ) noexcept {
    return running_rtsp_sessions == 0 && active_rtsp_sessions == 0;
  }

  constexpr bool expired_launch_should_clear_rtsp_active(
    unsigned int active_rtsp_sessions
  ) noexcept {
    return active_rtsp_sessions == 0;
  }

  struct admission_state_policy_t {
    bool cancelled = false;
    bool claimed = false;

    constexpr bool can_start() const noexcept {
      return !cancelled && !claimed;
    }

    constexpr bool mark_claimed() noexcept {
      if (!can_start()) {
        return false;
      }
      claimed = true;
      return true;
    }

    // A timeout loses to an admission that already completed. An explicit
    // user cancel retires both pending and claimed admissions; the caller then
    // stops any session that the winner published.
    constexpr bool retire(bool explicit_cancel) noexcept {
      if (claimed && !explicit_cancel) {
        return false;
      }
      cancelled = true;
      return true;
    }
  };

  enum class cleanup_acquire_result_e {
    acquired,
    superseded_by_launch,
    timed_out,
  };

  class coordinator_t {
  private:
    enum class owner_e {
      idle,
      launch,
      cleanup,
    };

  public:
    class launch_lease_t {
    public:
      launch_lease_t() = default;
      launch_lease_t(const launch_lease_t &) = delete;
      launch_lease_t &operator=(const launch_lease_t &) = delete;

      launch_lease_t(launch_lease_t &&other) noexcept:
          coordinator_ {std::exchange(other.coordinator_, nullptr)},
          generation_ {std::exchange(other.generation_, 0)},
          committed_ {std::exchange(other.committed_, false)} {
      }

      launch_lease_t &operator=(launch_lease_t &&other) noexcept {
        if (this != &other) {
          reset();
          coordinator_ = std::exchange(other.coordinator_, nullptr);
          generation_ = std::exchange(other.generation_, 0);
          committed_ = std::exchange(other.committed_, false);
        }
        return *this;
      }

      ~launch_lease_t() {
        reset();
      }

      explicit operator bool() const noexcept {
        return coordinator_ != nullptr;
      }

      void commit() noexcept {
        if (!coordinator_ || committed_) {
          return;
        }
        committed_ = true;
        coordinator_->commit_launch(generation_);
      }

      void mark_started() noexcept {
        if (coordinator_) {
          coordinator_->mark_launch_started(generation_);
        }
      }

      void reset() noexcept {
        if (!coordinator_) {
          return;
        }
        auto *coordinator = std::exchange(coordinator_, nullptr);
        const auto generation = std::exchange(generation_, 0);
        const bool committed = std::exchange(committed_, false);
        coordinator->release_launch(generation, committed);
      }

    private:
      friend class coordinator_t;

      launch_lease_t(coordinator_t *coordinator, std::uint64_t generation) noexcept:
          coordinator_ {coordinator},
          generation_ {generation} {
      }

      coordinator_t *coordinator_ = nullptr;
      std::uint64_t generation_ = 0;
      bool committed_ = false;
    };

    class cleanup_lease_t {
    public:
      cleanup_lease_t() = default;
      cleanup_lease_t(const cleanup_lease_t &) = delete;
      cleanup_lease_t &operator=(const cleanup_lease_t &) = delete;

      cleanup_lease_t(cleanup_lease_t &&other) noexcept:
          coordinator_ {std::exchange(other.coordinator_, nullptr)} {
      }

      cleanup_lease_t &operator=(cleanup_lease_t &&other) noexcept {
        if (this != &other) {
          reset();
          coordinator_ = std::exchange(other.coordinator_, nullptr);
        }
        return *this;
      }

      ~cleanup_lease_t() {
        reset();
      }

      explicit operator bool() const noexcept {
        return coordinator_ != nullptr;
      }

      void reset() noexcept {
        if (!coordinator_) {
          return;
        }
        auto *coordinator = std::exchange(coordinator_, nullptr);
        coordinator->release_cleanup();
      }

    private:
      friend class coordinator_t;

      explicit cleanup_lease_t(coordinator_t *coordinator) noexcept:
          coordinator_ {coordinator} {
      }

      coordinator_t *coordinator_ = nullptr;
    };

    struct cleanup_acquisition_t {
      cleanup_acquire_result_e result = cleanup_acquire_result_e::timed_out;
      std::optional<cleanup_lease_t> lease;
      bool deferred_cleanup_pending = false;
    };

    std::optional<launch_lease_t> acquire_launch(std::chrono::milliseconds timeout) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      std::unique_lock lock {mutex_};
      // Another HTTP launch/preparation owns the slot. Do not spend a strict
      // client's connection budget waiting behind it: the caller can return a
      // retryable conflict immediately. Only last-session cleanup is bounded.
      if (owner_ == owner_e::launch) {
        return std::nullopt;
      }
      const auto available = [this]() {
        // Give an already-waiting cleanup deterministic priority. Otherwise a
        // stream of HTTP retries could starve the old session's restoration.
        return owner_ == owner_e::idle && cleanup_waiters_ == 0;
      };
      if (!cv_.wait_until(lock, deadline, available)) {
        return std::nullopt;
      }

      owner_ = owner_e::launch;
      active_launch_committed_ = false;
      active_launch_started_ = false;
      active_launch_generation_ = ++next_launch_generation_;
      return launch_lease_t {this, active_launch_generation_};
    }

    cleanup_acquisition_t acquire_cleanup(std::chrono::milliseconds timeout) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      std::unique_lock lock {mutex_};
      ++cleanup_waiters_;
      const auto finish_wait = [this]() {
        --cleanup_waiters_;
        cv_.notify_all();
      };

      for (;;) {
        if (owner_ == owner_e::idle) {
          owner_ = owner_e::cleanup;
          finish_wait();
          cleanup_acquisition_t result;
          result.result = cleanup_acquire_result_e::acquired;
          result.deferred_cleanup_pending = std::exchange(deferred_cleanup_pending_, false);
          // Construct in coordinator_t's friend context. optional::emplace()
          // would perform the access check inside std::optional and therefore
          // cannot call cleanup_lease_t's private constructor.
          result.lease = cleanup_lease_t {this};
          return result;
        }

        if (owner_ == owner_e::launch) {
          // Launch preparation arrived first. Never wait here: acquire_cleanup
          // runs on the single RTSP handler while reaping a STOPPING session,
          // and blocking that handler would prevent the new client's ANNOUNCE
          // from being accepted. Failed launch paths revert their partial
          // display state while still holding the lease.
          if (!active_launch_started_) {
            deferred_cleanup_pending_ = true;
          }
          finish_wait();
          return {.result = cleanup_acquire_result_e::superseded_by_launch};
        }

        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
          // Re-evaluate once at the deadline so a simultaneous release
          // receives deterministic treatment.
          if (owner_ == owner_e::idle) {
            continue;
          }
          finish_wait();
          return {.result = cleanup_acquire_result_e::timed_out};
        }
      }
    }

  private:
    void commit_launch(std::uint64_t generation) noexcept {
      std::lock_guard lock {mutex_};
      if (owner_ == owner_e::launch && active_launch_generation_ == generation) {
        active_launch_committed_ = true;
        cv_.notify_all();
      }
    }

    void mark_launch_started(std::uint64_t generation) noexcept {
      std::lock_guard lock {mutex_};
      if (owner_ == owner_e::launch && active_launch_generation_ == generation) {
        active_launch_started_ = true;
        // The running successor inherits process-global streaming state. Its
        // own eventual last-session teardown now owns restoration.
        deferred_cleanup_pending_ = false;
        cv_.notify_all();
      }
    }

    void release_launch(std::uint64_t generation, bool committed) noexcept {
      std::lock_guard lock {mutex_};
      if (owner_ != owner_e::launch || active_launch_generation_ != generation) {
        return;
      }
      // committed is intentionally only diagnostic validation here. commit()
      // publishes the state before the HTTP handler hands the session to RTSP.
      // A lease destroyed without commit simply lets a waiting cleanup win.
      if (!committed) {
        active_launch_committed_ = false;
      }
      owner_ = owner_e::idle;
      active_launch_generation_ = 0;
      active_launch_started_ = false;
      cv_.notify_all();
    }

    void release_cleanup() noexcept {
      std::lock_guard lock {mutex_};
      if (owner_ == owner_e::cleanup) {
        owner_ = owner_e::idle;
        cv_.notify_all();
      }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    owner_e owner_ = owner_e::idle;
    std::size_t cleanup_waiters_ = 0;
    std::uint64_t next_launch_generation_ = 0;
    std::uint64_t active_launch_generation_ = 0;
    bool active_launch_committed_ = false;
    bool active_launch_started_ = false;
    bool deferred_cleanup_pending_ = false;
  };

  inline coordinator_t &coordinator() {
    // Intentionally process-lifetime storage: leases may be released by RTSP
    // objects during global shutdown, after ordinary function statics would
    // otherwise be vulnerable to destruction-order races.
    static auto *instance = new coordinator_t();
    return *instance;
  }

}  // namespace stream::lifecycle
