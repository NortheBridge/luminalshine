#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace changelog {
  struct UserState {
    std::vector<std::string> seen_installed_versions;
    std::string dismissed_update_version;
  };

  // Canonical release key: optional leading v removed; all other bytes retained.
  std::string normalize_version(std::string_view version);
  bool valid_version(std::string_view version);
  // These operations serialize read/modify/write and recover the atomic backup.
  // I/O/corrupt state errors throw; callers must not treat them as unseen state.
  UserState load(const std::filesystem::path &path, const std::string &username);
  UserState acknowledge(const std::filesystem::path &path, const std::string &username,
                        const std::string &version, bool installed);
}  // namespace changelog
