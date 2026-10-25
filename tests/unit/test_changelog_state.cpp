#include "src/changelog_state.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
  class ChangelogStateTest: public testing::Test {
  protected:
    std::filesystem::path directory;
    std::filesystem::path path;

    void SetUp() override {
      directory = std::filesystem::temp_directory_path() /
                  ("luminal-changelog-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::filesystem::create_directories(directory);
      path = directory / "state.json";
    }

    void TearDown() override {
      std::error_code ec;
      std::filesystem::remove_all(directory, ec);
    }
  };

  TEST_F(ChangelogStateTest, PersistsAcrossReadsAndSeparatesUsersAndAcknowledgementKinds) {
    EXPECT_TRUE(changelog::load(path, "alice.admin").seen_installed_versions.empty());
    changelog::acknowledge(path, "Alice.Admin", "26.10.0", false);
    auto state = changelog::load(path, "alice.admin");
    EXPECT_EQ(state.dismissed_update_version, "26.10.0");
    EXPECT_TRUE(state.seen_installed_versions.empty());

    changelog::acknowledge(path, "alice.admin", "26.10.0", true);
    changelog::acknowledge(path, "alice.admin", "26.10.0", true);
    state = changelog::load(path, "ALICE.ADMIN");
    EXPECT_EQ(state.seen_installed_versions, (std::vector<std::string> {"26.10.0"}));
    EXPECT_EQ(state.dismissed_update_version, "26.10.0");
    EXPECT_TRUE(changelog::load(path, "bob").seen_installed_versions.empty());
    EXPECT_TRUE(changelog::load(path, "bob").dismissed_update_version.empty());
  }

  TEST_F(ChangelogStateTest, ConcurrentAcknowledgementsDoNotOverwriteEachOther) {
    std::vector<std::thread> writers;
    for (int i = 0; i < 8; ++i) {
      writers.emplace_back([&, i] {
        EXPECT_NO_THROW(changelog::acknowledge(path, "admin", "26.10." + std::to_string(i), true));
      });
    }
    for (auto &writer : writers) writer.join();
    EXPECT_EQ(changelog::load(path, "admin").seen_installed_versions.size(), 8);
  }

  TEST_F(ChangelogStateTest, RecoversBackupWithoutLosingAcknowledgements) {
    changelog::acknowledge(path, "admin", "26.10.0", true);
    std::ofstream(path, std::ios::trunc) << "broken json";
    EXPECT_EQ(changelog::load(path, "admin").seen_installed_versions,
              (std::vector<std::string> {"26.10.0"}));
  }

  TEST_F(ChangelogStateTest, CorruptStateFailsRatherThanSilentlyLosingSeenVersions) {
    std::ofstream(path) << "broken json";
    EXPECT_THROW(changelog::load(path, "admin"), std::runtime_error);
    EXPECT_THROW(changelog::acknowledge(path, "admin", "26.10.0", true), std::runtime_error);
    std::ifstream file(path);
    std::string contents;
    std::getline(file, contents);
    EXPECT_EQ(contents, "broken json");
  }

  TEST_F(ChangelogStateTest, RejectsUnboundedOrNonVersionInputBeforeWriting) {
    EXPECT_THROW(changelog::acknowledge(path, "admin", std::string(97, '1'), true), std::invalid_argument);
    EXPECT_THROW(changelog::acknowledge(path, "admin", "../26.10.0", true), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(changelog::normalize_version("v26.10.0-beta.1"), "26.10.0-beta.1");
    EXPECT_TRUE(changelog::valid_version("26.10.0-beta.1+abc"));
    EXPECT_FALSE(changelog::valid_version(""));
  }
}  // namespace
