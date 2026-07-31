/**
 * @file src/yuv444_fallback.h
 * @brief Pure decision logic for "may this session give up YUV 4:4:4?"
 */
#pragma once

namespace video {

  /// What to do about YUV 4:4:4 after an encoder build failed in flight.
  enum class yuv444_fallback_e {
    not_applicable,  ///< Not a 4:4:4 session — there is nothing to fall back from.
    keep_444,  ///< The display is riding out a bounded outage; the failure is not about chroma.
    try_420,  ///< Nothing says the display is coming back; retry at 4:2:0.
  };

  /**
   * @brief The session-lifetime YUV 4:4:4 -> 4:2:0 fallback, made conditional
   * and revocable.
   *
   * The fallback exists for hardware that advertises 4:4:4 and then refuses it —
   * a TV that cannot accept 4:4:4, an encoder that fails only on the 4:4:4 pixel
   * format. It writes config.chromaSamplingType = 0 on the SESSION-LIFETIME
   * config and tells the client the effective chroma changed. Nothing ever puts
   * 4:4:4 back: before the display-recovery work the session died on the same
   * failure, so the downgrade only ever had to outlive a session that was ending
   * anyway, and the client renegotiated 4:4:4 on reconnect.
   *
   * Holding the session open across a GPU outage removes that guarantee, and
   * turns the fallback into a capability loss. During an outage EVERY encoder
   * build fails — 4:4:4 and 4:2:0 alike, because the GPU is down, not because of
   * chroma — so the first failure of a multi-minute outage is guaranteed to trip
   * it. The session then survives, exactly as intended, and streams 4:2:0 for the
   * rest of its life after an event the system fully recovered from. A transient
   * must not be able to cost a capability.
   *
   * Two rules keep that from happening, and neither weakens the real fallback:
   *
   *  1. Consult the recovery verdict FIRST. While the capture source says the
   *     display is coming back (platf::display_t::capture_recovering()), a failed
   *     encoder build is evidence about the GPU, not about 4:4:4, so 4:4:4 is
   *     kept and the caller waits the outage out instead. Only a display that is
   *     genuinely not coming back can cost the session its chroma.
   *
   *  2. The downgrade is PROVISIONAL until the 4:2:0 retry proves it helped. The
   *     verdict is published by the capture thread and read by the encoder
   *     thread, so the encoder can reach its failure a beat before the capture
   *     loop has sampled the ring and said "recovering" — rule 1 alone loses that
   *     race. But a 4:2:0 retry that fails too settles it after the fact: the
   *     failure was never about chroma, so 4:4:4 goes back and the client is
   *     never told anything. A retry that succeeds is exactly the hardware case
   *     the fallback exists for, and it commits, as today.
   *
   * Not thread-safe: one per encoder build attempt, on that thread.
   */
  class yuv444_fallback_t {
  public:
    /**
     * @param chroma_sampling_type config_t::chromaSamplingType (1 == YUV 4:4:4).
     *        Held by reference for the lifetime of this object, which is one
     *        encoder build attempt.
     */
    explicit yuv444_fallback_t(int &chroma_sampling_type):
        _chroma(&chroma_sampling_type) {
    }

    /**
     * @brief Decide what to do, applying a provisional downgrade if warranted.
     * @param capture_recovering What platf::display_t::capture_recovering() says.
     * @return try_420 when the caller should retry at 4:2:0 (the config is now
     *         4:2:0 and MUST be settled); keep_444 or not_applicable otherwise,
     *         with the config untouched.
     */
    [[nodiscard]] yuv444_fallback_e attempt(bool capture_recovering) {
      if (*_chroma != kYuv444) {
        return yuv444_fallback_e::not_applicable;
      }
      if (capture_recovering) {
        return yuv444_fallback_e::keep_444;
      }
      *_chroma = kYuv420;
      _provisional = true;
      return yuv444_fallback_e::try_420;
    }

    /**
     * @brief Report whether the 4:2:0 retry worked, and keep or revoke the
     *        downgrade accordingly.
     * @param retry_succeeded Whether the caller's 4:2:0 retry produced a working
     *        encoder.
     * @return true when the downgrade STANDS — and only then must the caller
     *         tell the client (mail::chroma_downgrade) and log it. False means
     *         4:4:4 has been restored and nothing happened as far as the rest of
     *         the system is concerned.
     */
    [[nodiscard]] bool settle(bool retry_succeeded) {
      if (!_provisional) {
        return false;
      }
      _provisional = false;
      if (retry_succeeded) {
        return true;
      }
      *_chroma = kYuv444;
      return false;
    }

    /// True while a downgrade has been applied but not yet settled.
    [[nodiscard]] bool provisional() const {
      return _provisional;
    }

  private:
    static constexpr int kYuv420 = 0;
    static constexpr int kYuv444 = 1;

    int *_chroma;
    bool _provisional = false;
  };

}  // namespace video
