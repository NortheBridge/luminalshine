/**
 * @file src/encoder_probe_shield.h
 * @brief Reusable C++ exception shield for encoder-probe call sites.
 *
 * The encoder probe in video.cpp drives FFmpeg / NVENC / AMF / QuickSync
 * encoder initialisation against the active display adapter. Graphics-driver
 * faults during that init have historically taken down the LuminalShine
 * process (see PR #21 for the AMD AMF hevc_amf incident); on Windows the
 * driver fault arrives as an SEH access violation and is handled by a
 * separate __try/__except trampoline in video.cpp. Independent of that,
 * a C++ exception thrown from validate_encoder (e.g. std::bad_alloc, an
 * FFmpeg wrapper throwing) should also be contained instead of unwinding
 * the entire probe loop.
 *
 * This header is intentionally header-only and free of side effects so
 * that unit tests can call run_cpp_exception_shield() directly with stub
 * probes — no display, no FFmpeg, no platform integration required.
 */
#pragma once

#include <functional>
#include <string>

namespace video::probe_shield {

  enum class outcome_e {
    ok,                ///< probe ran to completion (probe_returned holds its result)
    std_exception,     ///< probe threw a std::exception (std_what holds e.what())
    unknown_exception, ///< probe threw a non-std exception
  };

  struct result_t {
    outcome_e outcome;
    bool probe_returned;  ///< only meaningful when outcome == ok
    std::string std_what; ///< only populated when outcome == std_exception
  };

  /**
   * @brief Run a probe callable inside a C++ exception shield.
   *
   * Catches both std::exception (capturing what()) and unknown exceptions.
   * Does NOT shield against Windows SEH faults — those are handled by the
   * separate __try/__except trampoline in video.cpp because SEH wrappers
   * cannot host C++ unwind targets in the same frame.
   */
  inline result_t run_cpp_exception_shield(const std::function<bool()> &probe) {
    try {
      const bool ret = probe();
      return {outcome_e::ok, ret, {}};
    } catch (const std::exception &e) {
      return {outcome_e::std_exception, false, e.what()};
    } catch (...) {
      return {outcome_e::unknown_exception, false, {}};
    }
  }

}  // namespace video::probe_shield
