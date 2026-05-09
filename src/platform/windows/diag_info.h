/**
 * @file src/platform/windows/diag_info.h
 * @brief Declarations for the Windows-only diagnostics-info helpers used by
 *        the /api/metadata endpoint to populate the "About" page in the web UI.
 *
 * Each function in this header performs a one-shot Windows-API lookup and
 * returns the smallest plain-data structure that fully describes the result.
 * All functions are designed to fail gracefully (returning empty/default
 * values) so a partial Windows install or a denied registry permission can't
 * take down the metadata response.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace platf::diag {

  /**
   * @brief Information about an installed graphics driver, derived from the
   *        Windows display-class registry entries (idiomatic across vendors).
   */
  struct gpu_driver_info_t {
    uint32_t vendor_id = 0;     ///< PCI vendor ID (e.g. 0x10DE for NVIDIA, 0x1002/0x1022 for AMD, 0x8086 for Intel).
    uint32_t device_id = 0;     ///< PCI device ID.
    std::string description;    ///< User-visible adapter name (e.g. "NVIDIA GeForce RTX 5080").
    std::string driver_version; ///< e.g. "32.0.15.7270".
    std::string driver_date;    ///< ISO 8601 date string (e.g. "2026-04-22"). Empty if unavailable.
  };

  /**
   * @brief HDR / advanced-color state for a single active display path.
   */
  struct hdr_state_t {
    std::string display_name;      ///< GDI device name, e.g. "\\\\.\\DISPLAY1".
    std::string friendly_name;     ///< Optional monitor friendly name (e.g. "LG OLED C1 65"").
    bool advanced_color_supported = false;  ///< True if the path can drive HDR.
    bool advanced_color_enabled = false;    ///< True if HDR is currently active.
  };

  /**
   * @brief Windows Insider Preview channel information. is_insider == false
   *        means the host is on a Release / GA build.
   */
  struct insider_info_t {
    bool is_insider = false;
    std::string branch_name;     ///< e.g. "Canary", "Dev", "Beta", "ReleasePreview".
    std::string ring;            ///< e.g. "External" (any Insider) vs absent (GA).
    std::string content_type;    ///< e.g. "Mainline".
  };

  /**
   * @brief Enumerate installed graphics adapters and their driver metadata.
   *
   * Reads from the Windows display-class registry (Class GUID
   * {4d36e968-e325-11ce-bfc1-08002be10318}) using SetupDi APIs. Each entry
   * corresponds to one display adapter present in Device Manager. Adapters
   * that fail to enumerate are skipped silently.
   *
   * @return One element per detected adapter; possibly empty.
   */
  std::vector<gpu_driver_info_t> query_gpu_drivers();

  /**
   * @brief Enumerate active display outputs and their HDR support / state.
   *
   * Uses QueryDisplayConfig + DisplayConfigGetDeviceInfo with
   * DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO. Returns one entry per
   * active path (physical or virtual). The same display config API stalls
   * during a post-TDR window — failures here are treated as transient and
   * the function returns an empty vector instead of partial data.
   */
  std::vector<hdr_state_t> query_hdr_states();

  /**
   * @brief Detect whether the host is on a Windows Insider Preview build and,
   *        if so, which channel.
   *
   * Reads `HKLM\\SOFTWARE\\Microsoft\\WindowsSelfHost\\Applicability`. On a
   * Release / GA host this key is absent or has no `BranchName` and the
   * function returns is_insider == false.
   */
  insider_info_t query_insider_channel();

  /**
   * @brief Driver version string for the currently-installed virtual display
   *        driver (SudoVDA or MTT VDD), if any.
   *
   * Looks up the display-class registry entry whose Service value matches the
   * known SudoVDA / MTT service names and returns its DriverVersion. Returns
   * std::nullopt if no virtual display driver is installed or the lookup
   * fails.
   */
  std::optional<std::string> query_virtual_display_driver_version();

}  // namespace platf::diag
