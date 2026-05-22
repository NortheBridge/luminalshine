/**
 * @file src/platform/windows/nvprefs/driver_settings.cpp
 * @brief Definitions for nvidia driver settings.
 */
// this include
#include "driver_settings.h"

// local includes
#include "nvprefs_common.h"

namespace {

  // NVIDIA driver per-application profile keyed on the executable filename.
  // The profile name is the *NVAPI profile* identifier; the path is what the
  // driver matches against running processes. Both rename to LuminalShine.
  // First-run code in src/main.cpp clones any pre-existing `SunshineStream`
  // profile (keyed on `sunshine.exe`) into this new profile so users' GPU
  // power-mode / threaded-optimisations choices survive the migration.
  const auto sunshine_application_profile_name = L"LuminalShineStream";
  const auto sunshine_application_path = L"luminalshine.exe";
  // Legacy identifiers, kept so the migration step can locate and delete
  // the prior profile after copying its values across.
  const auto legacy_sunshine_application_profile_name = L"SunshineStream";
  const auto legacy_sunshine_application_path = L"sunshine.exe";

  void nvapi_error_message(NvAPI_Status status) {
    NvAPI_ShortString message = {};
    NvAPI_GetErrorMessage(status, message);
    nvprefs::error_message(std::string("NvAPI error: ") + message);
  }

  void fill_nvapi_string(NvAPI_UnicodeString &dest, const wchar_t *src) {
    static_assert(sizeof(NvU16) == sizeof(wchar_t));
    memcpy_s(dest, NVAPI_UNICODE_STRING_MAX * sizeof(NvU16), src, (wcslen(src) + 1) * sizeof(wchar_t));
  }

}  // namespace

namespace nvprefs {

  driver_settings_t::~driver_settings_t() {
    if (session_handle) {
      NvAPI_DRS_DestroySession(session_handle);
    }
  }

  bool driver_settings_t::init() {
    if (session_handle) {
      return true;
    }

    NvAPI_Status status;

    status = NvAPI_Initialize();
    if (status != NVAPI_OK) {
      info_message("NvAPI_Initialize() failed, ignore if you don't have NVIDIA video card");
      return false;
    }

    status = NvAPI_DRS_CreateSession(&session_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_CreateSession() failed");
      return false;
    }

    return load_settings();
  }

  void driver_settings_t::destroy() {
    if (session_handle) {
      NvAPI_DRS_DestroySession(session_handle);
      session_handle = nullptr;
    }
    NvAPI_Unload();
  }

  bool driver_settings_t::load_settings() {
    if (!session_handle) {
      return false;
    }

    NvAPI_Status status = NvAPI_DRS_LoadSettings(session_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_LoadSettings() failed");
      destroy();
      return false;
    }

    return true;
  }

  bool driver_settings_t::save_settings() {
    if (!session_handle) {
      return false;
    }

    NvAPI_Status status = NvAPI_DRS_SaveSettings(session_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_SaveSettings() failed");
      return false;
    }

    return true;
  }

  bool driver_settings_t::restore_global_profile_to_undo(const undo_data_t &undo_data) {
    if (!session_handle) {
      return false;
    }

    const auto &swapchain_data = undo_data.get_opengl_swapchain();
    if (swapchain_data) {
      NvAPI_Status status;

      NvDRSProfileHandle profile_handle = nullptr;
      status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_GetBaseProfile() failed");
        return false;
      }

      NVDRS_SETTING setting = {};
      setting.version = NVDRS_SETTING_VER;
      status = NvAPI_DRS_GetSetting(session_handle, profile_handle, OGL_CPL_PREFER_DXPRESENT_ID, &setting);

      if (status == NVAPI_OK && setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && setting.u32CurrentValue == swapchain_data->our_value) {
        if (swapchain_data->undo_value) {
          setting = {};
          setting.version = NVDRS_SETTING_VER1;
          setting.settingId = OGL_CPL_PREFER_DXPRESENT_ID;
          setting.settingType = NVDRS_DWORD_TYPE;
          setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
          setting.u32CurrentValue = *swapchain_data->undo_value;

          status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);

          if (status != NVAPI_OK) {
            nvapi_error_message(status);
            error_message("NvAPI_DRS_SetSetting() OGL_CPL_PREFER_DXPRESENT failed");
            return false;
          }
        } else {
          status = NvAPI_DRS_DeleteProfileSetting(session_handle, profile_handle, OGL_CPL_PREFER_DXPRESENT_ID);

          if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
            nvapi_error_message(status);
            error_message("NvAPI_DRS_DeleteProfileSetting() OGL_CPL_PREFER_DXPRESENT failed");
            return false;
          }
        }

        info_message("Restored OGL_CPL_PREFER_DXPRESENT for base profile");
      } else if (status == NVAPI_OK || status == NVAPI_SETTING_NOT_FOUND) {
        info_message("OGL_CPL_PREFER_DXPRESENT has been changed from our value in base profile, not restoring");
      } else {
        error_message("NvAPI_DRS_GetSetting() OGL_CPL_PREFER_DXPRESENT failed");
        return false;
      }
    }

    return true;
  }

  bool driver_settings_t::check_and_modify_global_profile(std::optional<undo_data_t> &undo_data) {
    if (!session_handle) {
      return false;
    }

    undo_data.reset();
    NvAPI_Status status;

    if (!get_nvprefs_options().opengl_vulkan_on_dxgi) {
      // User requested to leave OpenGL/Vulkan DXGI swapchain setting alone
      return true;
    }

    NvDRSProfileHandle profile_handle = nullptr;
    status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetBaseProfile() failed");
      return false;
    }

    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER;
    status = NvAPI_DRS_GetSetting(session_handle, profile_handle, OGL_CPL_PREFER_DXPRESENT_ID, &setting);

    // Remember current OpenGL/Vulkan DXGI swapchain setting and change it if needed
    if (status == NVAPI_SETTING_NOT_FOUND || (status == NVAPI_OK && setting.u32CurrentValue != OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED)) {
      undo_data = undo_data_t();
      if (status == NVAPI_OK) {
        undo_data->set_opengl_swapchain(OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED, setting.u32CurrentValue);
      } else {
        undo_data->set_opengl_swapchain(OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED, std::nullopt);
      }

      setting = {};
      setting.version = NVDRS_SETTING_VER1;
      setting.settingId = OGL_CPL_PREFER_DXPRESENT_ID;
      setting.settingType = NVDRS_DWORD_TYPE;
      setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
      setting.u32CurrentValue = OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED;

      status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_SetSetting() OGL_CPL_PREFER_DXPRESENT failed");
        return false;
      }

      info_message("Changed OGL_CPL_PREFER_DXPRESENT to OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED for base profile");
    } else if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetSetting() OGL_CPL_PREFER_DXPRESENT failed");
      return false;
    }

    return true;
  }

  bool driver_settings_t::check_and_modify_application_profile(bool &modified) {
    if (!session_handle) {
      return false;
    }

    modified = false;
    NvAPI_Status status;

    NvAPI_UnicodeString profile_name = {};
    fill_nvapi_string(profile_name, sunshine_application_profile_name);

    NvDRSProfileHandle profile_handle = nullptr;
    status = NvAPI_DRS_FindProfileByName(session_handle, profile_name, &profile_handle);

    if (status != NVAPI_OK) {
      // Create application profile if missing
      NVDRS_PROFILE profile = {};
      profile.version = NVDRS_PROFILE_VER1;
      fill_nvapi_string(profile.profileName, sunshine_application_profile_name);
      status = NvAPI_DRS_CreateProfile(session_handle, &profile, &profile_handle);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_CreateProfile() failed");
        return false;
      }
      modified = true;
    }

    NvAPI_UnicodeString sunshine_path = {};
    fill_nvapi_string(sunshine_path, sunshine_application_path);

    NVDRS_APPLICATION application = {};
    application.version = NVDRS_APPLICATION_VER_V1;
    status = NvAPI_DRS_GetApplicationInfo(session_handle, profile_handle, sunshine_path, &application);

    if (status != NVAPI_OK) {
      // Add application to application profile if missing
      application.version = NVDRS_APPLICATION_VER_V1;
      application.isPredefined = 0;
      fill_nvapi_string(application.appName, sunshine_application_path);
      fill_nvapi_string(application.userFriendlyName, sunshine_application_path);
      fill_nvapi_string(application.launcher, L"");

      status = NvAPI_DRS_CreateApplication(session_handle, profile_handle, &application);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_CreateApplication() failed");
        return false;
      }
      modified = true;
    }

    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER1;
    status = NvAPI_DRS_GetSetting(session_handle, profile_handle, PREFERRED_PSTATE_ID, &setting);

    if (!get_nvprefs_options().sunshine_high_power_mode) {
      if (status == NVAPI_OK &&
          setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION) {
        // User requested to not use high power mode for luminalshine.exe,
        // remove the setting from application profile if it's been set previously

        status = NvAPI_DRS_DeleteProfileSetting(session_handle, profile_handle, PREFERRED_PSTATE_ID);
        if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
          nvapi_error_message(status);
          error_message("NvAPI_DRS_DeleteProfileSetting() PREFERRED_PSTATE failed");
          return false;
        }
        modified = true;

        info_message(std::wstring(L"Removed PREFERRED_PSTATE for ") + sunshine_application_path);
      }
    } else if (status != NVAPI_OK ||
               setting.settingLocation != NVDRS_CURRENT_PROFILE_LOCATION ||
               setting.u32CurrentValue != PREFERRED_PSTATE_PREFER_MAX) {
      // Set power setting if needed
      setting = {};
      setting.version = NVDRS_SETTING_VER1;
      setting.settingId = PREFERRED_PSTATE_ID;
      setting.settingType = NVDRS_DWORD_TYPE;
      setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
      setting.u32CurrentValue = PREFERRED_PSTATE_PREFER_MAX;

      status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_SetSetting() PREFERRED_PSTATE failed");
        return false;
      }
      modified = true;

      info_message(std::wstring(L"Changed PREFERRED_PSTATE to PREFERRED_PSTATE_PREFER_MAX for ") + sunshine_application_path);
    }

    return true;
  }

  bool driver_settings_t::cleanup_legacy_application_profile(bool &removed) {
    // Best-effort migration: when LuminalShine starts for the first time
    // after the 26.05.1 rename, an `SunshineStream` NVAPI profile keyed on
    // `sunshine.exe` may still exist from the prior install. The fresh
    // `LuminalShineStream` profile (created/updated above by
    // `check_and_modify_application_profile`) carries the same configured
    // settings, so the legacy profile is now redundant. Delete it so users
    // don't see a phantom entry for a non-existent executable in the
    // NVIDIA Control Panel.
    //
    // Returns true on success or when nothing needed to be removed.
    // Failure to delete is logged but never blocks the caller — the new
    // profile is already in place, so leaving the orphan around is at
    // worst cosmetic.
    removed = false;
    if (!session_handle) {
      return false;
    }

    NvAPI_UnicodeString legacy_profile_name = {};
    fill_nvapi_string(legacy_profile_name, legacy_sunshine_application_profile_name);

    NvDRSProfileHandle legacy_handle = nullptr;
    NvAPI_Status status = NvAPI_DRS_FindProfileByName(session_handle, legacy_profile_name, &legacy_handle);
    if (status == NVAPI_PROFILE_NOT_FOUND) {
      return true;  // nothing to clean up
    }
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      info_message(L"Could not look up legacy SunshineStream profile during migration; leaving it in place.");
      return false;
    }

    status = NvAPI_DRS_DeleteProfile(session_handle, legacy_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      info_message(L"Could not delete legacy SunshineStream profile during migration; leaving it in place.");
      return false;
    }

    removed = true;
    info_message(std::wstring(L"Removed legacy NVAPI profile '") +
                 legacy_sunshine_application_profile_name +
                 L"' (keyed on " + legacy_sunshine_application_path + L").");
    return true;
  }

}  // namespace nvprefs
