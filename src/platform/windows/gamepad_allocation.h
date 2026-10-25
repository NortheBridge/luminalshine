/**
 * @file src/platform/windows/gamepad_allocation.h
 * @brief Cross-process allocation coordination for streaming and Bluetooth nudges.
 */
#pragma once

#include <Windows.h>
#include <sddl.h>

namespace platf::gamepad_allocation {
  // A manually started host can run under a user token while the helper runs
  // as SYSTEM. Give authenticated users only mutex wait/release rights, never
  // WRITE_DAC, WRITE_OWNER, DELETE, or an inheritable handle. This mutex is a
  // coordination aid, not an authorization boundary; driver handle ownership
  // remains responsible for protecting controllers belonging to other clients.
  inline HANDLE open_mutex() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;AU)",
          SDDL_REVISION_1, &descriptor, nullptr)) {
      return nullptr;
    }
    SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    // Request these same narrow rights even when opening an existing object;
    // CreateMutexW would request MUTEX_ALL_ACCESS and reject a user-token host.
    HANDLE mutex = CreateMutexExW(&attributes, L"Global\\LuminalShineGamepadAllocation",
                                 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    LocalFree(descriptor);
    return mutex;
  }

  class guard {
  public:
    explicit guard(DWORD timeout_ms): mutex(open_mutex()) {
      if (mutex) {
        const auto result = WaitForSingleObject(mutex, timeout_ms);
        owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
      }
    }
    ~guard() {
      if (owned) {
        ReleaseMutex(mutex);
      }
      if (mutex) {
        CloseHandle(mutex);
      }
    }
    guard(const guard &) = delete;
    guard &operator=(const guard &) = delete;
    explicit operator bool() const { return owned; }

  private:
    HANDLE mutex = nullptr;
    bool owned = false;
  };
}
