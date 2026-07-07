/**
 * @file src/platform/windows/diag_info.cpp
 * @brief Windows-only diagnostic-info helpers consumed by the /api/metadata
 *        endpoint to populate the web UI's About page. See diag_info.h.
 */
#include "diag_info.h"

// platform includes — order matters for SetupAPI / DisplayConfig headers.
#include <Windows.h>

#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <wingdi.h>

#include <algorithm>
#include <cwchar>
#include <sstream>

#include <boost/log/trivial.hpp>

#include "src/logging.h"
#include "src/platform/windows/misc.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")

namespace platf::diag {

  namespace {

    /**
     * Read a REG_SZ / REG_EXPAND_SZ value from an open registry key, returning
     * std::nullopt if the value doesn't exist or isn't a string. The output is
     * UTF-8 — the registry stores wide strings, we transcode at the boundary.
     */
    std::optional<std::string> read_reg_string(HKEY key, const wchar_t *name) {
      DWORD type = 0;
      DWORD size = 0;
      if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      if ((type != REG_SZ && type != REG_EXPAND_SZ) || size == 0) {
        return std::nullopt;
      }
      // Allocate enough room for the trailing NUL Windows is permitted to
      // omit on some keys.
      std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
      DWORD reread_size = size;
      if (RegQueryValueExW(
            key,
            name,
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(buffer.data()),
            &reread_size)
          != ERROR_SUCCESS) {
        return std::nullopt;
      }
      // Trim trailing NULs.
      while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
      }
      if (buffer.empty()) {
        return std::nullopt;
      }
      return platf::to_utf8(buffer);
    }

    /**
     * Read a FILETIME (REG_BINARY 8 bytes) value and return it as ISO 8601
     * date string (YYYY-MM-DD). DriverDate in the display-class registry is
     * stored as a FILETIME representing midnight UTC of the driver's date.
     */
    std::optional<std::string> read_reg_driver_date(HKEY key) {
      DWORD type = 0;
      DWORD size = 0;
      if (RegQueryValueExW(key, L"DriverDate", nullptr, &type, nullptr, &size)
          != ERROR_SUCCESS) {
        // Fall back to the human-readable string form some drivers also write.
        return read_reg_string(key, L"DriverDateData");
      }
      // Common case: the registry actually has it as REG_SZ in M-D-YYYY form.
      if (type == REG_SZ || type == REG_EXPAND_SZ) {
        auto raw = read_reg_string(key, L"DriverDate");
        if (!raw) {
          return std::nullopt;
        }
        // Reformat M-D-YYYY → YYYY-MM-DD if possible; otherwise return raw.
        std::string s = *raw;
        std::replace(s.begin(), s.end(), '/', '-');
        int month = 0, day = 0, year = 0;
        char tail = '\0';
        if (std::sscanf(s.c_str(), "%d-%d-%d%c", &month, &day, &year, &tail) >= 3
            && month >= 1 && month <= 12
            && day >= 1 && day <= 31
            && year >= 1900) {
          char iso[16] {};
          std::snprintf(iso, sizeof(iso), "%04d-%02d-%02d", year, month, day);
          return std::string(iso);
        }
        return s;
      }
      // Less common: REG_BINARY FILETIME form.
      if (type == REG_BINARY && size == sizeof(FILETIME)) {
        FILETIME ft {};
        DWORD reread = size;
        if (RegQueryValueExW(
              key,
              L"DriverDate",
              nullptr,
              &type,
              reinterpret_cast<LPBYTE>(&ft),
              &reread)
            != ERROR_SUCCESS) {
          return std::nullopt;
        }
        SYSTEMTIME st {};
        if (!FileTimeToSystemTime(&ft, &st)) {
          return std::nullopt;
        }
        char iso[16] {};
        std::snprintf(iso, sizeof(iso), "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
        return std::string(iso);
      }
      return std::nullopt;
    }

    /**
     * Parse a MatchingDeviceID like "PCI\VEN_10DE&DEV_2C02&..." (or the
     * lowercase variant) and extract the vendor + device IDs. Returns
     * std::nullopt for non-PCI hardware (e.g. virtual display drivers using
     * a ROOT\\ instance ID).
     */
    struct pci_ids_t {
      uint32_t vendor_id = 0;
      uint32_t device_id = 0;
    };
    std::optional<pci_ids_t> parse_pci_ids(const std::wstring &matching_id) {
      // Normalize to upper-case for case-insensitive scan.
      std::wstring upper(matching_id);
      std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);
      auto ven_pos = upper.find(L"VEN_");
      auto dev_pos = upper.find(L"DEV_");
      if (ven_pos == std::wstring::npos || dev_pos == std::wstring::npos) {
        return std::nullopt;
      }
      pci_ids_t ids;
      try {
        ids.vendor_id = static_cast<uint32_t>(
          std::stoul(upper.substr(ven_pos + 4, 4), nullptr, 16));
        ids.device_id = static_cast<uint32_t>(
          std::stoul(upper.substr(dev_pos + 4, 4), nullptr, 16));
      } catch (...) {
        return std::nullopt;
      }
      return ids;
    }

  }  // namespace

  std::vector<gpu_driver_info_t> query_gpu_drivers() {
    std::vector<gpu_driver_info_t> result;

    HDEVINFO dev_info = SetupDiGetClassDevsW(
      &GUID_DEVCLASS_DISPLAY,
      nullptr,
      nullptr,
      DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE) {
      BOOST_LOG(debug) << "diag: SetupDiGetClassDevs(DISPLAY) failed err=" << GetLastError();
      return result;
    }

    SP_DEVINFO_DATA dev_data {};
    dev_data.cbSize = sizeof(dev_data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(dev_info, index, &dev_data); ++index) {
      gpu_driver_info_t info;

      // MatchingDeviceID lives in the device's HARDWARE registry section, but
      // the simpler portable path is to ask SetupAPI for the InstanceId and
      // use that as the device-ID source. SetupDiGetDeviceRegistryProperty
      // with SPDRP_HARDWAREID returns a MULTI_SZ list whose first entry has
      // the same VEN_/DEV_ structure.
      DWORD reg_data_type = 0;
      wchar_t hwid_buf[512] {};
      if (SetupDiGetDeviceRegistryPropertyW(
            dev_info,
            &dev_data,
            SPDRP_HARDWAREID,
            &reg_data_type,
            reinterpret_cast<PBYTE>(hwid_buf),
            sizeof(hwid_buf),
            nullptr)) {
        if (auto ids = parse_pci_ids(hwid_buf)) {
          info.vendor_id = ids->vendor_id;
          info.device_id = ids->device_id;
        }
      }

      // DriverDesc — the human-friendly adapter name. Lives in the per-driver
      // registry key opened below; SPDRP_DEVICEDESC is also a fallback.
      wchar_t desc_buf[256] {};
      if (SetupDiGetDeviceRegistryPropertyW(
            dev_info,
            &dev_data,
            SPDRP_DEVICEDESC,
            &reg_data_type,
            reinterpret_cast<PBYTE>(desc_buf),
            sizeof(desc_buf),
            nullptr)) {
        info.description = platf::to_utf8(std::wstring(desc_buf));
      }

      // Driver registry key holds DriverVersion + DriverDate. SetupDi opens
      // this for us so we never have to hard-code the {4d36e968-...}\NNNN
      // path.
      HKEY drv_key = SetupDiOpenDevRegKey(
        dev_info,
        &dev_data,
        DICS_FLAG_GLOBAL,
        0,
        DIREG_DRV,
        KEY_READ);
      if (drv_key != INVALID_HANDLE_VALUE) {
        if (auto v = read_reg_string(drv_key, L"DriverVersion")) {
          info.driver_version = *v;
        }
        if (auto d = read_reg_driver_date(drv_key)) {
          info.driver_date = *d;
        }
        // DriverDesc is the canonical name; prefer it over the device-desc
        // fallback when present.
        if (auto desc = read_reg_string(drv_key, L"DriverDesc")) {
          info.description = *desc;
        }
        RegCloseKey(drv_key);
      }

      // Skip pure software / non-PCI display devices that have no PCI IDs and
      // no version data — they're noise on this list (Microsoft Basic Render
      // Driver, Remote Desktop Mirror, etc.).
      if (info.vendor_id == 0 && info.driver_version.empty()) {
        continue;
      }

      result.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(dev_info);
    return result;
  }

  std::vector<hdr_state_t> query_hdr_states() {
    std::vector<hdr_state_t> result;

    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
      return result;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(
          QDC_ONLY_ACTIVE_PATHS,
          &path_count,
          paths.data(),
          &mode_count,
          modes.data(),
          nullptr)
        != ERROR_SUCCESS) {
      return result;
    }
    paths.resize(path_count);

    for (const auto &path : paths) {
      hdr_state_t state;

      // Source GDI name — the \\.\DISPLAY# string.
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
      source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      source_name.header.size = sizeof(source_name);
      source_name.header.adapterId = path.sourceInfo.adapterId;
      source_name.header.id = path.sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&source_name.header) == ERROR_SUCCESS) {
        state.display_name = platf::to_utf8(std::wstring(source_name.viewGdiDeviceName));
      }

      // Target friendly name (monitor's reported name, e.g. "LG OLED C1 65").
      DISPLAYCONFIG_TARGET_DEVICE_NAME target_name {};
      target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      target_name.header.size = sizeof(target_name);
      target_name.header.adapterId = path.targetInfo.adapterId;
      target_name.header.id = path.targetInfo.id;
      if (DisplayConfigGetDeviceInfo(&target_name.header) == ERROR_SUCCESS
          && target_name.monitorFriendlyDeviceName[0] != L'\0') {
        state.friendly_name = platf::to_utf8(std::wstring(target_name.monitorFriendlyDeviceName));
      }

      // Advanced color (HDR) — supported and currently-enabled flags.
      DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color_info {};
      color_info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
      color_info.header.size = sizeof(color_info);
      color_info.header.adapterId = path.targetInfo.adapterId;
      color_info.header.id = path.targetInfo.id;
      if (DisplayConfigGetDeviceInfo(&color_info.header) == ERROR_SUCCESS) {
        state.advanced_color_supported = color_info.advancedColorSupported != 0;
        state.advanced_color_enabled = color_info.advancedColorEnabled != 0;
      }

      // Skip paths that contributed no usable identifying info — these tend
      // to be ghost entries left over from a recently-detached output.
      if (state.display_name.empty() && state.friendly_name.empty()) {
        continue;
      }
      result.push_back(std::move(state));
    }
    return result;
  }

  insider_info_t query_insider_channel() {
    insider_info_t out;

    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\WindowsSelfHost\\Applicability",
      0,
      KEY_READ,
      &key);
    if (status != ERROR_SUCCESS || key == nullptr) {
      // Either WindowsSelfHost isn't present (true on a Release / GA host)
      // or we lack permission. Either way, treat as non-Insider.
      return out;
    }

    if (auto branch = read_reg_string(key, L"BranchName")) {
      out.branch_name = std::move(*branch);
    }
    if (auto ring = read_reg_string(key, L"Ring")) {
      out.ring = std::move(*ring);
    }
    if (auto content = read_reg_string(key, L"ContentType")) {
      out.content_type = std::move(*content);
    }
    RegCloseKey(key);

    // Heuristic: a non-empty BranchName combined with Ring == "External" is
    // the unambiguous Insider Preview signature on current Windows 11. Some
    // older Insider builds don't write Ring, so a non-empty BranchName alone
    // also flips us into Insider mode.
    out.is_insider = !out.branch_name.empty();
    return out;
  }

  std::optional<std::string> query_virtual_display_driver_version() {
    // Service names match the driver INF: SudoVDA registers as "SudoVDA".
    static const wchar_t *kKnownServices[] = {
      L"SudoVDA"
    };

    HDEVINFO dev_info = SetupDiGetClassDevsW(
      &GUID_DEVCLASS_DISPLAY,
      nullptr,
      nullptr,
      DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE) {
      return std::nullopt;
    }

    std::optional<std::string> found;
    SP_DEVINFO_DATA dev_data {};
    dev_data.cbSize = sizeof(dev_data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(dev_info, index, &dev_data); ++index) {
      // Read the Service value first — fast filter to avoid the registry
      // open for every adapter.
      DWORD reg_data_type = 0;
      wchar_t service_buf[64] {};
      if (!SetupDiGetDeviceRegistryPropertyW(
            dev_info,
            &dev_data,
            SPDRP_SERVICE,
            &reg_data_type,
            reinterpret_cast<PBYTE>(service_buf),
            sizeof(service_buf),
            nullptr)) {
        continue;
      }
      bool matched = false;
      for (auto known : kKnownServices) {
        if (_wcsicmp(service_buf, known) == 0) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        continue;
      }

      HKEY drv_key = SetupDiOpenDevRegKey(
        dev_info,
        &dev_data,
        DICS_FLAG_GLOBAL,
        0,
        DIREG_DRV,
        KEY_READ);
      if (drv_key == INVALID_HANDLE_VALUE) {
        continue;
      }
      if (auto v = read_reg_string(drv_key, L"DriverVersion")) {
        found = std::move(*v);
      }
      RegCloseKey(drv_key);
      if (found) {
        break;
      }
    }
    SetupDiDestroyDeviceInfoList(dev_info);
    return found;
  }

}  // namespace platf::diag
