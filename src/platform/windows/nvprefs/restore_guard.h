/**
 * @file src/platform/windows/nvprefs/restore_guard.h
 * @brief Restore driver preferences before logging/runtime teardown, on every exit path.
 */
#pragma once

#include <cstdio>

namespace nvprefs {
  // Keep the lifecycle policy independent of NVAPI so interrupted-startup and
  // failed-restore paths can be tested without modifying a machine's settings.
  template<class Preferences>
  class restore_guard {
  public:
    explicit restore_guard(Preferences &preferences) noexcept:
        preferences_(preferences) {}

    restore_guard(const restore_guard &) = delete;
    restore_guard &operator=(const restore_guard &) = delete;

    ~restore_guard() noexcept {
      bool restored = true;
      try {
        if (preferences_.owning_undo_file()) {
          restored = preferences_.load() && preferences_.restore_global_profile();
        }
      } catch (...) {
        restored = false;
      }
      // Always release NVAPI, even if loading/restoration threw. Failed
      // restoration must leave the undo journal available to the next startup.
      try {
        preferences_.unload();
      } catch (...) {
        restored = false;
      }
      if (!restored) {
        // Never turn a shutdown failure into a second exception from logging.
        std::fputs("NVIDIA preference cleanup failed; any remaining undo file is retained for the next startup.\n", stderr);
      }
    }

  private:
    Preferences &preferences_;
  };
}  // namespace nvprefs
