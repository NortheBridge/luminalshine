/**
 * @file src/platform/windows/dir_acl.cpp
 * @brief See dir_acl.h.
 *
 * The SDDL string is parsed by ConvertStringSecurityDescriptorToSecurityDescriptorW
 * to avoid hand-rolling explicit access entries. We apply DACL and SACL in
 * separate SetNamedSecurityInfoW calls so a missing ACCESS_SYSTEM_SECURITY
 * privilege (typical outside the service context) downgrades cleanly to
 * DACL-only enforcement rather than failing the whole call.
 */
#include "src/platform/windows/dir_acl.h"

#include <windows.h>
#include <sddl.h>
#include <aclapi.h>

#include "src/logging.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf {

  namespace {

    constexpr auto kConfigDirSddl =
      L"D:P"
      L"(A;OICI;FA;;;SY)"      // SYSTEM full, container+object inherit
      L"(A;OICI;FA;;;BA)"      // BUILTIN\Administrators full, inherit
      L"(A;OICI;FRFX;;;BU)"    // BUILTIN\Users read+execute, inherit
      L"S:(ML;OICI;NW;;;SI)";  // mandatory label, System IL, no-write-up

    struct security_descriptor_guard {
      PSECURITY_DESCRIPTOR sd {nullptr};
      ~security_descriptor_guard() {
        if (sd) {
          LocalFree(sd);
        }
      }
    };

    void log_win_error(const char *what, DWORD err) {
      BOOST_LOG(warning) << "dir_acl: "sv << what << " failed (err="sv << err << ")"sv;
    }

  }  // namespace

  bool harden_config_directory(const std::filesystem::path &dir) {
    if (dir.empty()) {
      return false;
    }

    security_descriptor_guard sdg {};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kConfigDirSddl,
          SDDL_REVISION_1,
          &sdg.sd,
          nullptr)) {
      log_win_error("ConvertStringSecurityDescriptor", GetLastError());
      return false;
    }

    PACL dacl = nullptr;
    BOOL dacl_present = FALSE, dacl_defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(sdg.sd, &dacl_present, &dacl, &dacl_defaulted) || !dacl_present) {
      log_win_error("GetSecurityDescriptorDacl", GetLastError());
      return false;
    }

    PACL sacl = nullptr;
    BOOL sacl_present = FALSE, sacl_defaulted = FALSE;
    GetSecurityDescriptorSacl(sdg.sd, &sacl_present, &sacl, &sacl_defaulted);

    const std::wstring wpath = dir.wstring();
    auto *path_arg = const_cast<LPWSTR>(wpath.c_str());

    bool ok = true;

    // DACL first. Failure here is meaningful — without WRITE_DAC the file
    // remains unprotected and the caller should know.
    DWORD rc = SetNamedSecurityInfoW(
      path_arg,
      SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr,
      nullptr,
      dacl,
      nullptr);
    if (rc != ERROR_SUCCESS) {
      log_win_error("SetNamedSecurityInfo(DACL)", rc);
      ok = false;
    }

    // SACL (mandatory label) second. Requires ACCESS_SYSTEM_SECURITY; the
    // service token gets it via SeSecurityPrivilege, but dev/test contexts
    // typically don't. Treat failure as a soft warning so non-service
    // runs (e.g. `luminalshine.exe --help`) don't spam errors.
    if (sacl_present && sacl) {
      rc = SetNamedSecurityInfoW(
        path_arg,
        SE_FILE_OBJECT,
        LABEL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,
        sacl);
      if (rc != ERROR_SUCCESS) {
        BOOST_LOG(info) << "dir_acl: SetNamedSecurityInfo(LABEL) skipped (err="sv << rc
                        << "); DACL is the load-bearing protection."sv;
      }
    }

    return ok;
  }

}  // namespace platf
