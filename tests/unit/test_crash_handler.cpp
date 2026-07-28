/**
 * @file tests/unit/test_crash_handler.cpp
 * @brief Test src/crash_handler.*.
 *
 * Only the dump-directory pruning helper is exercised here. The actual
 * exception filter / terminate handler are deliberately NOT tested
 * in-process: triggering them would tear the test runner down.
 */
#include "../tests_common.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <src/crash_handler.h>
#include <string>
#include <vector>

namespace {
  namespace fs = std::filesystem;

  class CrashHandlerPruneTest: public testing::Test {
  protected:
    void SetUp() override {
      test_dir = fs::temp_directory_path() / ("crash_handler_prune_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
      fs::create_directories(test_dir);
    }

    void TearDown() override {
      std::error_code ec;
      fs::remove_all(test_dir, ec);
    }

    /// Create a file and stamp it with a deterministic mtime, `age_rank`
    /// seconds in the past — rank 0 is the newest. Explicit stamps avoid
    /// flakiness from filesystem timestamp resolution.
    fs::path make_file(const std::string &name, int age_rank) {
      const auto path = test_dir / name;
      std::ofstream {path} << "fake dump";
      fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::seconds {age_rank * 10});
      return path;
    }

    std::vector<std::string> remaining_files() const {
      std::vector<std::string> names;
      for (const auto &entry : fs::directory_iterator {test_dir}) {
        names.push_back(entry.path().filename().string());
      }
      std::sort(names.begin(), names.end());
      return names;
    }

    fs::path test_dir;
  };

  TEST_F(CrashHandlerPruneTest, KeepsNewestFive) {
    // dump-0 is the newest, dump-8 the oldest.
    for (int i = 0; i < 9; ++i) {
      make_file("dump-" + std::to_string(i) + ".dmp", i);
    }

    EXPECT_EQ(crash_handler::prune_dumps(test_dir, 5), 4u);

    const std::vector<std::string> expected {"dump-0.dmp", "dump-1.dmp", "dump-2.dmp", "dump-3.dmp", "dump-4.dmp"};
    EXPECT_EQ(remaining_files(), expected);
  }

  TEST_F(CrashHandlerPruneTest, NoopAtOrUnderLimit) {
    for (int i = 0; i < 5; ++i) {
      make_file("dump-" + std::to_string(i) + ".dmp", i);
    }

    EXPECT_EQ(crash_handler::prune_dumps(test_dir, 5), 0u);
    EXPECT_EQ(remaining_files().size(), 5u);
  }

  TEST_F(CrashHandlerPruneTest, IgnoresNonDumpFiles) {
    // Six dumps (one over the limit) plus two bystanders older than all
    // of them. Only the oldest .dmp may go; the bystanders must survive
    // and must not count toward the limit.
    for (int i = 0; i < 6; ++i) {
      make_file("dump-" + std::to_string(i) + ".dmp", i);
    }
    make_file("notes.txt", 20);
    make_file("dump-archive.dmp.bak", 21);

    EXPECT_EQ(crash_handler::prune_dumps(test_dir, 5), 1u);

    const auto names = remaining_files();
    EXPECT_EQ(names.size(), 7u);
    EXPECT_EQ(std::count(names.begin(), names.end(), "dump-5.dmp"), 0);
    EXPECT_EQ(std::count(names.begin(), names.end(), "notes.txt"), 1);
    EXPECT_EQ(std::count(names.begin(), names.end(), "dump-archive.dmp.bak"), 1);
  }

  TEST_F(CrashHandlerPruneTest, MissingDirectoryIsSafe) {
    EXPECT_EQ(crash_handler::prune_dumps(test_dir / "does_not_exist", 5), 0u);
  }

  TEST_F(CrashHandlerPruneTest, KeepZeroRemovesEverything) {
    for (int i = 0; i < 3; ++i) {
      make_file("dump-" + std::to_string(i) + ".dmp", i);
    }

    EXPECT_EQ(crash_handler::prune_dumps(test_dir, 0), 3u);
    EXPECT_TRUE(remaining_files().empty());
  }
}  // namespace
