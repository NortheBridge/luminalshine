/**
 * @file src/platform/windows/nvprefs/driver_settings.h
 * @brief Declarations for nvidia driver settings.
 */
#pragma once

// nvapi headers
// disable clang-format header reordering
// wrapper keeps NvAPI SAL annotations friendly to non-MSVC toolchains
// clang-format off
#include "../nvapi_driver_settings.h"
// clang-format on

// local includes
#include "undo_data.h"

namespace nvprefs {

  class driver_settings_t {
  public:
    ~driver_settings_t();

    bool init();

    void destroy();

    bool load_settings();

    bool save_settings();

    bool restore_global_profile_to_undo(const undo_data_t &undo_data);

    bool check_and_modify_global_profile(std::optional<undo_data_t> &undo_data);

    bool check_and_modify_application_profile(bool &modified);

    /**
     * @brief Remove the legacy `SunshineStream` NVAPI profile if present.
     *
     * One-time migration helper invoked after `check_and_modify_application_profile`
     * has created/updated the new `LuminalShineStream` profile. Settings the
     * profile carries are config-driven (re-applied to the new profile on
     * every start), so deleting the orphan profile loses nothing.
     *
     * @param[out] removed Set to true if a legacy profile was found and
     *                     deleted; false if no legacy profile existed.
     * @return true on success (including the "nothing to remove" case),
     *         false if the NVAPI call to delete the existing legacy
     *         profile failed.
     */
    bool cleanup_legacy_application_profile(bool &removed);

  private:
    NvDRSSessionHandle session_handle = nullptr;
  };

}  // namespace nvprefs
