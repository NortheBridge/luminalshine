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
#include <string>
#include <string_view>
#include <utility>

namespace platf::video_worker::ring_consumers {

  /**
   * How long a worker waits for a retiring holder of another client to release its ring.
   *
   * Holder teardown is bounded by the parent. In steady state the worker is
   * asked to shut down and given 2 s, then terminated and given 0.5 s more,
   * and then the pipe reader is joined. A worker that failed at startup is
   * terminated and given 3 s, then the display health is probed. The budget
   * sits above both. A reconnect of the holder's own client does not wait: it
   * takes the ring over at once.
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
        session_id_ {other.session_id_},
        token_ {other.token_} {
    }

    ring_lease_t &operator=(ring_lease_t &&other) noexcept {
      if (this != &other) {
        reset();
        registry_ = std::exchange(other.registry_, nullptr);
        session_id_ = other.session_id_;
        token_ = other.token_;
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

    ring_lease_t(registry_t *registry, std::uint64_t session_id, std::uint64_t token) noexcept:
        registry_ {registry},
        session_id_ {session_id},
        token_ {token} {
    }

    registry_t *registry_ = nullptr;
    std::uint64_t session_id_ = 0;
    std::uint64_t token_ = 0;  ///< Identifies this holder, so a superseded lease cannot release its successor.
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
     * - No holder: claim, and the caller is recorded as the holder.
     * - Holder of the same, identified client: claim. The caller takes the ring
     *   over, because a client that reconnects after dropping without a clean
     *   disconnect leaves a session that is not retiring until it times out.
     *   The superseded lease's release then leaves the new holder in place.
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
     * @param client_id The streaming client's identity. Empty when unknown; an
     *        unknown client never takes a ring over and is never taken over.
     */
    acquire_result_t try_acquire(
      std::uint64_t session_id,
      std::function<bool()> retiring,
      std::chrono::milliseconds waited,
      std::string_view client_id = {}
    ) {
      std::lock_guard lock {mutex_};
      const auto holder = holders_.find(session_id);
      if (holder != holders_.end()) {
        const bool holder_retiring = holder->second.retiring && holder->second.retiring();
        if (holder_retiring && waited < release_wait_budget) {
          return {decision_e::wait_for_release, {}};
        }
        const bool same_client = !client_id.empty() && holder->second.client_id == client_id;
        if (!same_client) {
          return {decision_e::use_safe_capture, {}};
        }
      }
      // A superseded holder whose session ends after its successor leaves no
      // record while its worker still reads; a joiner in that window may claim.
      // That needs both of one client's sessions to overlap a third client's join.
      const auto token = ++last_token_;
      holders_.insert_or_assign(session_id, holder_t {std::string {client_id}, std::move(retiring), token});
      return {decision_e::claim, ring_lease_t {this, session_id, token}};
    }

    bool is_held(std::uint64_t session_id) const {
      std::lock_guard lock {mutex_};
      return holders_.contains(session_id);
    }

  private:
    friend class ring_lease_t;

    struct holder_t {
      std::string client_id;
      std::function<bool()> retiring;
      std::uint64_t token;
    };

    void release(std::uint64_t session_id, std::uint64_t token) noexcept {
      std::lock_guard lock {mutex_};
      const auto holder = holders_.find(session_id);
      if (holder != holders_.end() && holder->second.token == token) {
        holders_.erase(holder);
      }
    }

    mutable std::mutex mutex_;
    std::map<std::uint64_t, holder_t> holders_;
    std::uint64_t last_token_ = 0;
  };

  inline void ring_lease_t::reset() noexcept {
    if (registry_ != nullptr) {
      std::exchange(registry_, nullptr)->release(session_id_, token_);
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
