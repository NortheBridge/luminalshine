/**
 * @file tests/unit/platform/windows/test_dir_acl.cpp
 * @brief Test the crash dump folder restriction in src/platform/windows/dir_acl.cpp.
 */
#include "../../../tests_common.h"

#ifdef _WIN32
  #include <algorithm>
  #include <filesystem>
  #include <fstream>
  #include <src/platform/windows/dir_acl.h>
  #include <string>
  #include <vector>

// clang-format off
  #include <windows.h>
  #include <aclapi.h>
  #include <sddl.h>
// clang-format on

namespace {
  namespace fs = std::filesystem;

  // What a folder under %ProgramData% typically inherits: every signed-in
  // user may write and Users may read. OI+CI so existing and new children
  // inherit it.
  constexpr auto kOpenAccessSddl = L"D:P(A;OICI;FA;;;AU)(A;OICI;FRFX;;;BU)";
  constexpr auto kOwnerRestoreSddl = L"D:P(A;OICI;FA;;;AU)";

  constexpr auto kSystemSid = L"S-1-5-18";
  constexpr auto kAdministratorsSid = L"S-1-5-32-544";
  constexpr auto kUsersSid = L"S-1-5-32-545";

  struct DaclReport {
    bool is_protected = false;
    std::vector<std::wstring> allowed_sids;  // sorted
  };

  /// Read the DACL of @p path: whether inheritance is cut, and the SID of every allow entry.
  bool read_dacl(const fs::path &path, DaclReport &report) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
      return false;
    }

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool ok = GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE;
    report.is_protected = (control & SE_DACL_PROTECTED) != 0;
    report.allowed_sids.clear();

    ACL_SIZE_INFORMATION size_info {};
    if (ok && dacl != nullptr) {
      ok = GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation) != FALSE;
    }
    for (DWORD i = 0; ok && dacl != nullptr && i < size_info.AceCount; ++i) {
      void *entry = nullptr;
      if (!GetAce(dacl, i, &entry)) {
        ok = false;
        break;
      }
      if (static_cast<ACE_HEADER *>(entry)->AceType != ACCESS_ALLOWED_ACE_TYPE) {
        continue;
      }
      auto *allowed = static_cast<ACCESS_ALLOWED_ACE *>(entry);
      LPWSTR text = nullptr;
      if (!ConvertSidToStringSidW(reinterpret_cast<PSID>(&allowed->SidStart), &text)) {
        ok = false;
        break;
      }
      report.allowed_sids.emplace_back(text);
      LocalFree(text);
    }
    LocalFree(descriptor);

    std::sort(report.allowed_sids.begin(), report.allowed_sids.end());
    return ok;
  }

  /// Replace the DACL of @p path with a protected one built from @p sddl; entries marked OI/CI reach existing children as inherited entries.
  bool apply_protected_dacl(const fs::path &path, const wchar_t *sddl) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
      return false;
    }

    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    bool ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present;
    if (ok) {
      std::wstring native = path.wstring();
      ok = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    }
    LocalFree(descriptor);
    return ok;
  }

  /// Give the signed-in users access back through the owner's right to change the DACL.
  void restore_access(const fs::path &path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kOwnerRestoreSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
      return;
    }
    SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
  }

  class CrashDumpDirAcl: public testing::Test {
  protected:
    void SetUp() override {
      if (test_env::scratch_dir().empty()) {
        GTEST_SKIP() << "test scratch directory is unavailable";
      }
      dir = test_env::scratch_dir() / "crash_dump_dir_acl";
      std::error_code ec;
      fs::remove_all(dir, ec);
      fs::create_directories(dir);

      // Set the DACL by path, so its entries reach children as inherited
      // ones, then create a file that picks them up.
      ASSERT_TRUE(apply_protected_dacl(dir, kOpenAccessSddl));
      child = dir / "existing.dmp";
      std::ofstream {child} << "dump";
      ASSERT_TRUE(fs::exists(child));
    }

    void TearDown() override {
      if (dir.empty()) {
        return;
      }
      // After the restriction only SYSTEM and Administrators have access;
      // the owner can still reset the DACL.
      restore_access(child);
      restore_access(dir);
      std::error_code ec;
      fs::remove_all(dir, ec);
    }

    fs::path dir;
    fs::path child;
  };

  TEST_F(CrashDumpDirAcl, RestrictsFolderAndExistingFilesToSystemAndAdministrators) {
    const std::vector<std::wstring> expected {kSystemSid, kAdministratorsSid};

    DaclReport before;
    ASSERT_TRUE(read_dacl(child, before));
    ASSERT_NE(std::find(before.allowed_sids.begin(), before.allowed_sids.end(), kUsersSid), before.allowed_sids.end()) << "precondition: the existing file inherits access for Users";

    ASSERT_TRUE(platf::restrict_crash_dump_directory(dir));

    DaclReport folder;
    ASSERT_TRUE(read_dacl(dir, folder));
    EXPECT_TRUE(folder.is_protected);
    EXPECT_EQ(folder.allowed_sids, expected);

    DaclReport file;
    ASSERT_TRUE(read_dacl(child, file));
    EXPECT_EQ(file.allowed_sids, expected);
  }
}  // namespace
#endif  // _WIN32
