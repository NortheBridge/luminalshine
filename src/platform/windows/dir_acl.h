/**
 * @file src/platform/windows/dir_acl.h
 * @brief Restrictive DACL + Mandatory Integrity Control hardening for the
 *        config directory under %ProgramData%\LuminalShine\.
 */
#pragma once

#include <filesystem>

namespace platf {

  /**
   * @brief Apply a protected DACL and a System-level mandatory integrity
   *        label to @p dir. Idempotent and self-repairing on every call;
   *        invoked once from appdata() at first resolution per process.
   *
   * Target DACL (PROTECTED, OI+CI inheritance):
   *   - NT AUTHORITY\SYSTEM             : Full
   *   - BUILTIN\Administrators          : Full
   *   - BUILTIN\Users                   : Read + Execute (traverse)
   *
   * Target SACL: mandatory label ACE binding contents to the System
   * integrity level with NO_WRITE_UP, so processes running below System
   * IL cannot write regardless of the DACL.
   *
   * Returns true on success. A false return is logged but does not
   * abort: dev/test contexts that do not run as SYSTEM cannot set the
   * SACL and will see a partial-success warning; the DACL portion is
   * still applied where the caller has WRITE_DAC.
   */
  bool harden_config_directory(const std::filesystem::path &dir);

}  // namespace platf
