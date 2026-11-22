#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/platform/windows/nvprefs/restore_guard.h"
#include "src/startup_display_wait.h"

namespace {
  struct preferences {
    std::vector<std::string> calls;
    bool undo = true;
    bool load_ok = true;
    bool restore_ok = true;
    bool throw_load = false;
    bool throw_restore = false;
    bool throw_unload = false;

    bool owning_undo_file() { return undo; }
    bool load() {
      calls.emplace_back("load");
      if (throw_load) throw std::runtime_error("load failed");
      return load_ok;
    }
    bool restore_global_profile() {
      calls.emplace_back("restore");
      if (throw_restore) throw std::runtime_error("restore failed");
      if (restore_ok) undo = false;
      return restore_ok;
    }
    void unload() {
      calls.emplace_back("unload");
      if (throw_unload) throw std::runtime_error("unload failed");
    }
  };

  TEST(NvPrefsRestoreGuard, InterruptedStartupRestoresBeforeLoggingDies) {
    preferences prefs;
    const auto startup = [&] {
      struct logging_lifetime {
        preferences &prefs;
        ~logging_lifetime() { prefs.calls.emplace_back("logging stopped"); }
      } logging {prefs};
      nvprefs::restore_guard guard {prefs};
      const startup::display_wait_callbacks callbacks {
        [] { return false; },
        [] { return true; },
        {}
      };
      const auto result = startup::wait_for_physical_display(12, std::chrono::milliseconds {250}, callbacks);
      if (result.aborted) return;
      FAIL() << "The startup shutdown request must abort the display wait";
    };
    startup();
    EXPECT_EQ(prefs.calls, (std::vector<std::string> {"load", "restore", "unload", "logging stopped"}));
    EXPECT_FALSE(prefs.undo);
  }

  TEST(NvPrefsRestoreGuard, NormalExitRestoresExactlyOnce) {
    preferences prefs;
    { nvprefs::restore_guard guard {prefs}; }
    EXPECT_EQ(prefs.calls, (std::vector<std::string> {"load", "restore", "unload"}));
    EXPECT_FALSE(prefs.undo);
  }

  TEST(NvPrefsRestoreGuard, NoOwnedUndoDoesNotLoadOrRestore) {
    preferences prefs;
    prefs.undo = false;
    { nvprefs::restore_guard guard {prefs}; }
    EXPECT_EQ(prefs.calls, (std::vector<std::string> {"unload"}));
  }

  TEST(NvPrefsRestoreGuard, FailedLoadKeepsUndoForNextStartup) {
    preferences prefs;
    prefs.load_ok = false;
    { nvprefs::restore_guard guard {prefs}; }
    EXPECT_EQ(prefs.calls, (std::vector<std::string> {"load", "unload"}));
    EXPECT_TRUE(prefs.undo);
  }

  TEST(NvPrefsRestoreGuard, FailedRestoreKeepsUndoAndStillUnloads) {
    preferences prefs;
    prefs.restore_ok = false;
    { nvprefs::restore_guard guard {prefs}; }
    EXPECT_EQ(prefs.calls, (std::vector<std::string> {"load", "restore", "unload"}));
    EXPECT_TRUE(prefs.undo);
  }

  TEST(NvPrefsRestoreGuard, CleanupExceptionsDoNotEscapeDuringUnwinding) {
    for (int stage = 0; stage < 3; ++stage) {
      preferences prefs;
      prefs.throw_load = stage == 0;
      prefs.throw_restore = stage == 1;
      prefs.throw_unload = stage == 2;
      EXPECT_THROW(([&] {
        nvprefs::restore_guard guard {prefs};
        throw std::logic_error("startup failed");
      }()), std::logic_error);
      ASSERT_FALSE(prefs.calls.empty());
      EXPECT_EQ(prefs.calls.back(), "unload");
      if (stage < 2) EXPECT_TRUE(prefs.undo);
    }
  }
}
