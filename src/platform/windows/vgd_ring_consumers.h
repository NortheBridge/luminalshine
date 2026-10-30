/**
 * @file src/platform/windows/vgd_ring_consumers.h
 * @brief Parent-side registry that keeps a LuminalVGD frame ring single-consumer.
 *
 * The driver's frame ring has exactly one consumer. When a second client joins
 * a running app on the same virtual display, a second isolated video worker
 * opening that ring steals frames from the first (the first drops to roughly
 * half its frame rate and the second times out). The parent records which live
 * worker consumes which ring so a joiner can start in isolated WGC safe-capture
 * instead.
 *
 * Standard library only, so it compiles and tests without the Windows runtime.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

namespace platf::video_worker::ring_consumers {

  /**
   * How long a worker waits for a retiring holder to release its ring.
   *
   * Holder teardown is bounded by the parent: the worker is asked to shut down
   * and given 2 s, then terminated and given 0.5 s more, and then the pipe
   * reader is joined. The budget sits above that worst case so a quick
   * reconnect of the same client still lands on the ring instead of WGC.
   */
  constexpr std::chrono::milliseconds release_wait_budget {5000};

  /** Suggested pause between attempts while a retiring holder is waited for. */
  constexpr std::chrono::milliseconds release_poll_interval {50};

  enum class decision_e {
    claim,  ///< The ring is free; the returned lease owns it.
    wait_for_release,  ///< The holder is retiring; retry shortly.
    use_safe_capture,  ///< The ring is consumed by a live worker; do not open it.
  };

  class registry_t;

  /**
   * @brief Move-only ownership of one ring; releases it exactly once.
   *
   * Must not outlive the registry that issued it.
   */
  class ring_lease_t {
  public:
    ring_lease_t() noexcept = default;

    ring_lease_t(const ring_lease_t &) = delete;
    ring_lease_t &operator=(const ring_lease_t &) = delete;

    ring_lease_t(ring_lease_t &&other) noexcept:
        registry_ {std::exchange(other.registry_, nullptr)},
        session_id_ {other.session_id_} {
    }

    ring_lease_t &operator=(ring_lease_t &&other) noexcept {
      if (this != &other) {
        reset();
        registry_ = std::exchange(other.registry_, nullptr);
        session_id_ = other.session_id_;
      }
      return *this;
    }

    ~ring_lease_t() {
      reset();
    }

    /** Release the ring now. Safe to call on an empty or already released lease. */
    void reset() noexcept;

    explicit operator bool() const noexcept {
      return registry_ != nullptr;
    }

  private:
    friend class registry_t;

    ring_lease_t(registry_t *registry, std::uint64_t session_id) noexcept:
        registry_ {registry},
        session_id_ {session_id} {
    }

    registry_t *registry_ = nullptr;
    std::uint64_t session_id_ = 0;
  };

  /**
   * @brief Thread-safe map from a ring's session id to the worker consuming it.
   */
  class registry_t {
  public:
    struct acquire_result_t {
      decision_e decision = decision_e::use_safe_capture;
      ring_lease_t lease;  ///< Non-empty only when decision is claim.
    };

    registry_t() = default;
    registry_t(const registry_t &) = delete;
    registry_t &operator=(const registry_t &) = delete;

    /**
     * Decide, atomically, whether the caller may consume the ring of @p session_id.
     *
     * - No holder: claim, and the caller's lease is recorded with @p retiring.
     * - Holder that is retiring and @p waited is under release_wait_budget:
     *   wait_for_release. The caller polls again with its updated @p waited.
     * - Otherwise: use_safe_capture.
     *
     * @param session_id Ring session id from the driver.
     * @param retiring Reports whether this holder has begun tearing down. It is
     *        called with the registry lock held by other callers, so it must be
     *        cheap and must not call back into the registry. An empty function
     *        never reports retiring.
     * @param waited How long the caller has already waited. Passed in so the
     *        policy needs no clock.
     */
    acquire_result_t try_acquire(std::uint64_t session_id, std::function<bool()> retiring, std::chrono::milliseconds waited) {
      std::lock_guard lock {mutex_};
      const auto holder = holders_.find(session_id);
      if (holder == holders_.end()) {
        holders_.emplace(session_id, std::move(retiring));
        return {decision_e::claim, ring_lease_t {this, session_id}};
      }
      const bool holder_retiring = holder->second && holder->second();
      if (holder_retiring && waited < release_wait_budget) {
        return {decision_e::wait_for_release, {}};
      }
      return {decision_e::use_safe_capture, {}};
    }

    bool is_held(std::uint64_t session_id) const {
      std::lock_guard lock {mutex_};
      return holders_.contains(session_id);
    }

  private:
    friend class ring_lease_t;

    void release(std::uint64_t session_id) noexcept {
      std::lock_guard lock {mutex_};
      holders_.erase(session_id);
    }

    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::function<bool()>> holders_;
  };

  inline void ring_lease_t::reset() noexcept {
    if (registry_ != nullptr) {
      std::exchange(registry_, nullptr)->release(session_id_);
    }
  }

  /**
   * The process-wide registry. Intentionally never destroyed: a worker thread
   * may still release a lease while the process shuts down.
   */
  inline registry_t &global() {
    static auto *const instance = new registry_t;
    return *instance;
  }

}  // namespace platf::video_worker::ring_consumers
