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
#include <wincrypt.h>
#include <wingdi.h>
#include <wintrust.h>

#include <softpub.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <boost/log/trivial.hpp>

#include "src/logging.h"
#include "src/platform/windows/misc.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wintrust.lib")

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
     * Case-insensitive membership test against a REG_MULTI_SZ block (also
     * accepts a plain REG_SZ, which walks as a one-element list).
     */
    bool multi_sz_contains(const wchar_t *multi, DWORD bytes, const wchar_t *needle) {
      const wchar_t *p = multi;
      const wchar_t *end = multi + bytes / sizeof(wchar_t);
      while (p < end && *p != L'\0') {
        if (_wcsicmp(p, needle) == 0) {
          return true;
        }
        p += wcslen(p) + 1;
      }
      return false;
    }

    /**
     * Directory of the running executable — the LuminalShine install root,
     * where the bundled driver package lives. Independent of the process
     * working directory.
     */
    std::optional<std::filesystem::path> executable_dir() {
      wchar_t buf[MAX_PATH] {};
      const DWORD len = GetModuleFileNameW(nullptr, buf, _countof(buf));
      if (len == 0 || len >= _countof(buf)) {
        return std::nullopt;
      }
      auto parent = std::filesystem::path(buf).parent_path();
      if (parent.empty()) {
        return std::nullopt;
      }
      return parent;
    }

    /**
     * Extract "DriverVer = MM/DD/YYYY,x.y.z.w" from an INF (ASCII/UTF-8;
     * the packaged LuminalVGD INF is stampinf output in that form). Fills
     * the version and ISO-date fields of `out`; leaves them absent on any
     * parse failure. First DriverVer line wins.
     */
    void parse_inf_driver_ver(const std::filesystem::path &inf_path, bundled_vgd_package_t &out) {
      std::ifstream in(inf_path);
      if (!in) {
        return;
      }
      std::string line;
      while (std::getline(in, line)) {
        const auto semi = line.find(';');
        if (semi != std::string::npos) {
          line.resize(semi);
        }
        const auto pos = line.find_first_not_of(" \t");
        if (pos == std::string::npos || _strnicmp(line.c_str() + pos, "DriverVer", 9) != 0) {
          continue;
        }
        const auto eq = line.find('=', pos + 9);
        if (eq == std::string::npos) {
          continue;
        }
        int month = 0;
        int day = 0;
        int year = 0;
        char ver[32] {};
        if (std::sscanf(line.c_str() + eq + 1, " %d/%d/%d ,%31[0-9.]", &month, &day, &year, ver) == 4 &&
            month >= 1 && month <= 12 && day >= 1 && day <= 31 && year >= 1900) {
          char iso[16] {};
          std::snprintf(iso, sizeof(iso), "%04d-%02d-%02d", year, month, day);
          out.date = std::string(iso);
          out.version = std::string(ver);
        }
        return;
      }
    }

    /**
     * Simple display name of the embedded Authenticode signer certificate,
     * or nullopt when the file has no embedded PKCS#7 signature.
     */
    std::optional<std::string> authenticode_signer(const std::filesystem::path &file) {
      HCERTSTORE cert_store = nullptr;
      HCRYPTMSG msg = nullptr;
      std::optional<std::string> signer;
      if (!CryptQueryObject(
            CERT_QUERY_OBJECT_FILE,
            file.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY,
            0,
            nullptr,
            nullptr,
            nullptr,
            &cert_store,
            &msg,
            nullptr)) {
        return std::nullopt;
      }
      DWORD info_size = 0;
      if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &info_size) && info_size > 0) {
        std::vector<BYTE> buf(info_size);
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, buf.data(), &info_size)) {
          const auto *signer_info = reinterpret_cast<const CMSG_SIGNER_INFO *>(buf.data());
          CERT_INFO want {};
          want.Issuer = signer_info->Issuer;
          want.SerialNumber = signer_info->SerialNumber;
          if (PCCERT_CONTEXT cert = CertFindCertificateInStore(
                cert_store,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                0,
                CERT_FIND_SUBJECT_CERT,
                &want,
                nullptr)) {
            wchar_t name[256] {};
            if (CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, _countof(name)) > 1) {
              signer = platf::to_utf8(std::wstring(name));
            }
            CertFreeCertificateContext(cert);
          }
        }
      }
      CryptMsgClose(msg);
      CertCloseStore(cert_store, 0);
      return signer;
    }

    /**
     * Offline Authenticode chain verification (no revocation, cached URL
     * retrieval only — the metadata endpoint must never block on network).
     */
    bool authenticode_trusted(const std::filesystem::path &file) {
      const std::wstring wpath = file.wstring();
      WINTRUST_FILE_INFO file_info {};
      file_info.cbStruct = sizeof(file_info);
      file_info.pcwszFilePath = wpath.c_str();

      WINTRUST_DATA wtd {};
      wtd.cbStruct = sizeof(wtd);
      wtd.dwUIChoice = WTD_UI_NONE;
      wtd.fdwRevocationChecks = WTD_REVOKE_NONE;
      wtd.dwUnionChoice = WTD_CHOICE_FILE;
      wtd.pFile = &file_info;
      wtd.dwStateAction = WTD_STATEACTION_VERIFY;
      wtd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

      GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
      const LONG status = WinVerifyTrust(nullptr, &action, &wtd);
      wtd.dwStateAction = WTD_STATEACTION_CLOSE;
      WinVerifyTrust(nullptr, &action, &wtd);
      return status == ERROR_SUCCESS;
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

  virtual_display_driver_info_t query_virtual_display_driver_info() {
    // Match on the devnode hardware ID. SPDRP_SERVICE cannot work here: for
    // a UMDF driver the devnode's associated service is always WUDFRd (the
    // reflector) — the INF's UmdfService name never appears in it, so a
    // service-name filter finds nothing (and never did for SudoVDA either).
    constexpr const wchar_t *kHardwareId = L"root\\luminal_vgd";

    virtual_display_driver_info_t out {};
    HDEVINFO dev_info = SetupDiGetClassDevsW(
      &GUID_DEVCLASS_DISPLAY,
      nullptr,
      nullptr,
      DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE) {
      return out;
    }

    SP_DEVINFO_DATA dev_data {};
    dev_data.cbSize = sizeof(dev_data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(dev_info, index, &dev_data); ++index) {
      DWORD reg_data_type = 0;
      // REG_MULTI_SZ block; keep two trailing NULs spare so the walk below
      // always terminates even on a truncated read.
      wchar_t hwid_buf[512] {};
      DWORD hwid_bytes = 0;
      if (!SetupDiGetDeviceRegistryPropertyW(
            dev_info,
            &dev_data,
            SPDRP_HARDWAREID,
            &reg_data_type,
            reinterpret_cast<PBYTE>(hwid_buf),
            sizeof(hwid_buf) - 2 * sizeof(wchar_t),
            &hwid_bytes)) {
        continue;
      }
      if (reg_data_type != REG_MULTI_SZ && reg_data_type != REG_SZ) {
        continue;
      }
      if (!multi_sz_contains(hwid_buf, hwid_bytes, kHardwareId)) {
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
      out.version = read_reg_string(drv_key, L"DriverVersion");
      out.date = read_reg_driver_date(drv_key);
      RegCloseKey(drv_key);
      if (out.version) {
        break;
      }
    }
    SetupDiDestroyDeviceInfoList(dev_info);
    return out;
  }

  std::optional<std::string> query_virtual_display_driver_version() {
    return query_virtual_display_driver_info().version;
  }

  bundled_vgd_package_t query_bundled_vgd_package() {
    bundled_vgd_package_t out {};
    const auto exe_dir = executable_dir();
    if (!exe_dir) {
      return out;
    }
    std::error_code ec;
    const auto pkg_dir = *exe_dir / L"drivers" / L"luminalvgd" / L"driver-package";
    const auto inf_path = pkg_dir / L"luminalvgd.inf";
    if (!std::filesystem::exists(inf_path, ec)) {
      return out;
    }
    out.present = true;
    parse_inf_driver_ver(inf_path, out);

    const auto dll_path = pkg_dir / L"luminal_vgd_driver.dll";
    if (std::filesystem::exists(dll_path, ec)) {
      if (auto signer = authenticode_signer(dll_path)) {
        out.signature = authenticode_trusted(dll_path) ?
                          *signer :
                          (*signer + " (unverified)");
      } else {
        out.signature = "Unsigned";
      }
    }
    return out;
  }

}  // namespace platf::diag
