# WiX 3 MSI build — hand-authored, CPack-WIX-decoupled.
#
# Replaces the prior CPACK_GENERATOR "WIX" setup as of PR2.c of the
# WiX 7 migration. CMake's CPack-WIX generator is hardwired to WiX 3
# and has no v4+ replacement upstream; owning the candle/light
# invocation directly here is the prerequisite for the v7 toolchain
# swap that lands in PR3-4.
#
# Pipeline (driven by `cmake --build <build> --target luminalshine_msi`):
#
#   1. STAGE  — `cmake --install <build> --component <comp>
#                --prefix <build>/wix_staging/<comp>` for every install
#               COMPONENT in LUMINALSHINE_INSTALL_COMPONENTS. Produces a
#               <staging>/<component>/<install-relative-paths>/<file>
#               layout that mirrors CPack's _CPack_Packages/win64/WIX/
#               tree exactly — required for scripts/gen_wix_files.py
#               to reproduce the byte-identical Component/File/Directory
#               Ids the table-diff oracle locks down.
#
#   2. GEN    — scripts/gen_wix_files.py walks the staged tree and
#               writes <build>/wix_gen/files.wxs (Directory hierarchy
#               + per-file Components with Guid="*") plus
#               <build>/wix_gen/features.wxs (ProductFeature /
#               CM_G_* / CM_C_* hierarchy + FeatureComponents
#               bindings, sourced from packaging/windows/wix/features.json).
#
#   3. CANDLE — compile every .wxs (luminalshine.wxs, custom_actions.wxs,
#               files.wxs, features.wxs) to a .wixobj.
#
#   4. LIGHT  — link the .wixobjs into the final MSI, with the WixUI /
#               WixUtilExtension / WixFirewallExtension extensions
#               loaded and the MyScripts + PayloadRoot bindpaths
#               pointing at the in-repo .vbs scripts and the
#               wix_payload tree (which the build's own targets
#               populate with the renamed luminalshinesvc.exe / xbox /
#               sessionmon binaries — see tools/CMakeLists.txt).
#
# Output: ${CMAKE_BINARY_DIR}/cpack_artifacts/LuminalShine-<ver>-win64.msi
#         (same directory the prior CPack-WIX path produced, so
#         build_bootstrapper.ps1 picks it up unchanged).
#
# CPack is still used for the portable ZIP — only the WIX generator
# is removed. ZIP-vs-WIX separation matches the prior split in
# .github/workflows/ci-windows.yml.

# Only generate the portable ZIP via CPack. The MSI is built by the
# luminalshine_msi target below.
set(CPACK_GENERATOR "ZIP")

# ----------------------------------------------------------------------------
# Sanitize version for WiX: must be x.x.x.x with integers [0,65534].
# Carried over verbatim from the prior CPack-WIX configuration. The
# resulting value is fed to candle via -dProductVersion= AND set as
# CPACK_PACKAGE_VERSION so the ZIP filename stays version-stamped.
# ----------------------------------------------------------------------------
set(_RAW_VER "${PROJECT_VERSION_NUMERIC}")
set(_WIX_MAJ 0)
set(_WIX_MIN 0)
set(_WIX_PAT 0)
set(_WIX_REV 0)

if(_RAW_VER MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  set(_WIX_MAJ "${CMAKE_MATCH_1}")
  set(_WIX_MIN "${CMAKE_MATCH_2}")
  set(_WIX_PAT "${CMAKE_MATCH_3}")
  set(_WIX_REV 0)
else()
  if(DEFINED CMAKE_PROJECT_VERSION_MAJOR)
    set(_WIX_MAJ "${CMAKE_PROJECT_VERSION_MAJOR}")
  endif()
  if(DEFINED CMAKE_PROJECT_VERSION_MINOR)
    set(_WIX_MIN "${CMAKE_PROJECT_VERSION_MINOR}")
  endif()
  if(DEFINED CMAKE_PROJECT_VERSION_PATCH)
    set(_WIX_PAT "${CMAKE_PROJECT_VERSION_PATCH}")
  endif()
  set(_WIX_REV 0)
endif()

foreach(_v IN ITEMS _WIX_MAJ _WIX_MIN _WIX_PAT _WIX_REV)
  if(${_v} GREATER 65534)
    set(${_v} 65534)
  endif()
endforeach()

set(LUMINALSHINE_MSI_VERSION "${_WIX_MAJ}.${_WIX_MIN}.${_WIX_PAT}.${_WIX_REV}")
set(CPACK_PACKAGE_VERSION "${LUMINALSHINE_MSI_VERSION}")
message(STATUS "LUMINALSHINE_MSI_VERSION = ${LUMINALSHINE_MSI_VERSION} (from ${PROJECT_VERSION_FULL})")

# ----------------------------------------------------------------------------
# WiX 3 toolchain discovery.
# Chocolatey's wixtoolset install sets %WIX% to the install root; the
# binaries live in $env:WIX\bin. Also try PATH as a fallback.
# ----------------------------------------------------------------------------
set(_wix_hints "")
if(DEFINED ENV{WIX})
  list(APPEND _wix_hints "$ENV{WIX}/bin")
endif()
find_program(LUMINALSHINE_CANDLE_EXE candle.exe HINTS ${_wix_hints} REQUIRED)
find_program(LUMINALSHINE_LIGHT_EXE  light.exe  HINTS ${_wix_hints} REQUIRED)
message(STATUS "WiX 3 candle: ${LUMINALSHINE_CANDLE_EXE}")
message(STATUS "WiX 3 light:  ${LUMINALSHINE_LIGHT_EXE}")

# Python 3 for scripts/gen_wix_files.py — required here, not QUIET (the
# build genuinely needs it to author files.wxs / features.wxs).
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ----------------------------------------------------------------------------
# Identity / paths.
# ----------------------------------------------------------------------------
# UpgradeCode is fixed — match the golden exactly; changing it breaks
# upgrade detection for every installed user.
set(LUMINALSHINE_UPGRADE_GUID "{C2C36624-2D9C-4AFD-9C79-6B7861AE4A0D}")

# ProductCode is per-build (CPack-WIX behaved the same way). The table
# diff normalizes ProductCode out, so a fresh UUID each configure is fine.
execute_process(
  COMMAND ${Python3_EXECUTABLE} -c "import uuid; print('{' + str(uuid.uuid4()).upper() + '}')"
  OUTPUT_VARIABLE LUMINALSHINE_PRODUCT_CODE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _uuid_rc
)
if(NOT _uuid_rc EQUAL 0)
  message(FATAL_ERROR "Failed to generate ProductCode via Python uuid")
endif()
message(STATUS "LUMINALSHINE_PRODUCT_CODE = ${LUMINALSHINE_PRODUCT_CODE}")

set(LUMINALSHINE_MSI_STAGING_DIR "${CMAKE_BINARY_DIR}/wix_staging")
set(LUMINALSHINE_MSI_WIXOBJ_DIR  "${CMAKE_BINARY_DIR}/wix_obj")
set(LUMINALSHINE_MSI_GEN_DIR     "${CMAKE_BINARY_DIR}/wix_gen")
set(LUMINALSHINE_MSI_OUT_DIR     "${CMAKE_BINARY_DIR}/cpack_artifacts")
set(LUMINALSHINE_MSI_OUT_FILE    "${LUMINALSHINE_MSI_OUT_DIR}/LuminalShine-${LUMINALSHINE_MSI_VERSION}-win64.msi")

set(LUMINALSHINE_FILES_WXS    "${LUMINALSHINE_MSI_GEN_DIR}/files.wxs")
set(LUMINALSHINE_FEATURES_WXS "${LUMINALSHINE_MSI_GEN_DIR}/features.wxs")

set(LUMINALSHINE_WXS_FIXED
  "${CMAKE_SOURCE_DIR}/packaging/windows/wix/luminalshine.wxs"
  "${CMAKE_SOURCE_DIR}/packaging/windows/wix/custom_actions.wxs"
)

# Install COMPONENTs that exist somewhere in the install() rules of
# this project. `Unspecified` is CMake's default bucket for install()
# rules with no COMPONENT keyword (currently just the common assets
# copy in cmake/packaging/common.cmake:22 — kept for parity with the
# CM_C_Unspecified Feature in the golden).
set(LUMINALSHINE_INSTALL_COMPONENTS
  application
  assets
  audio
  autostart
  dxgi
  firewall
  mttvdd
  sudovda
  Unspecified
)

# ----------------------------------------------------------------------------
# STAGE step — wipe + re-stage every install component in one command
# so the generator always sees a consistent tree. Stamp file is the
# tracked output for the rest of the pipeline.
# ----------------------------------------------------------------------------
set(LUMINALSHINE_STAGING_STAMP "${LUMINALSHINE_MSI_STAGING_DIR}/.stamped")
set(_stage_commands
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${LUMINALSHINE_MSI_STAGING_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LUMINALSHINE_MSI_STAGING_DIR}"
)
foreach(comp ${LUMINALSHINE_INSTALL_COMPONENTS})
  list(APPEND _stage_commands
    COMMAND ${CMAKE_COMMAND}
      --install "${CMAKE_BINARY_DIR}"
      --component "${comp}"
      --prefix "${LUMINALSHINE_MSI_STAGING_DIR}/${comp}"
  )
endforeach()
list(APPEND _stage_commands COMMAND ${CMAKE_COMMAND} -E touch "${LUMINALSHINE_STAGING_STAMP}")
add_custom_command(
  OUTPUT "${LUMINALSHINE_STAGING_STAMP}"
  ${_stage_commands}
  COMMENT "MSI staging: install all components to ${LUMINALSHINE_MSI_STAGING_DIR}"
  VERBATIM
)

# ----------------------------------------------------------------------------
# GEN step — run scripts/gen_wix_files.py against the staged tree.
# ----------------------------------------------------------------------------
add_custom_command(
  OUTPUT
    "${LUMINALSHINE_FILES_WXS}"
    "${LUMINALSHINE_FEATURES_WXS}"
  DEPENDS
    "${LUMINALSHINE_STAGING_STAMP}"
    "${CMAKE_SOURCE_DIR}/scripts/gen_wix_files.py"
    "${CMAKE_SOURCE_DIR}/packaging/windows/wix/features.json"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LUMINALSHINE_MSI_GEN_DIR}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/gen_wix_files.py"
    --staging "${LUMINALSHINE_MSI_STAGING_DIR}"
    --features-manifest "${CMAKE_SOURCE_DIR}/packaging/windows/wix/features.json"
    --files-out "${LUMINALSHINE_FILES_WXS}"
    --features-out "${LUMINALSHINE_FEATURES_WXS}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "MSI generate: files.wxs + features.wxs from staging tree"
  VERBATIM
)

# ----------------------------------------------------------------------------
# CANDLE step — compile each .wxs to a .wixobj.
# Extensions and -d preprocessor variables match what CPack-WIX
# passed today (see prior windows_wix.cmake history) so wxs sources
# that reference $(var.X) keep resolving.
# ----------------------------------------------------------------------------
set(_candle_extensions -ext WixUtilExtension -ext WixFirewallExtension)
set(_candle_dvars
  "-dProductCode=${LUMINALSHINE_PRODUCT_CODE}"
  "-dProductName=${CMAKE_PROJECT_NAME}"
  "-dProductVersion=${LUMINALSHINE_MSI_VERSION}"
  "-dManufacturer=${CPACK_PACKAGE_VENDOR}"
  "-dUpgradeCode=${LUMINALSHINE_UPGRADE_GUID}"
  "-dLicenseRtf=${CMAKE_SOURCE_DIR}/packaging/windows/LICENSE.rtf"
  "-dProductIcon=${CMAKE_SOURCE_DIR}/sunshine.ico"
  "-dLuminalShineAppId=${WINDOWS_APP_USER_MODEL_ID}"
  "-dBinDir=${CMAKE_BINARY_DIR}"
)

set(_wxs_all
  ${LUMINALSHINE_WXS_FIXED}
  "${LUMINALSHINE_FILES_WXS}"
  "${LUMINALSHINE_FEATURES_WXS}"
)

set(_wixobjs)
foreach(_wxs ${_wxs_all})
  get_filename_component(_base "${_wxs}" NAME_WE)
  set(_obj "${LUMINALSHINE_MSI_WIXOBJ_DIR}/${_base}.wixobj")
  add_custom_command(
    OUTPUT "${_obj}"
    DEPENDS "${_wxs}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LUMINALSHINE_MSI_WIXOBJ_DIR}"
    COMMAND "${LUMINALSHINE_CANDLE_EXE}" -nologo -arch x64
      -out "${_obj}"
      ${_candle_extensions}
      ${_candle_dvars}
      "${_wxs}"
    COMMENT "candle: ${_base}.wxs"
    VERBATIM
  )
  list(APPEND _wixobjs "${_obj}")
endforeach()

# ----------------------------------------------------------------------------
# LIGHT step — link wixobjs into the final MSI. Extensions and
# bindpaths mirror what the prior CPACK_WIX_LIGHT_EXTRA_FLAGS supplied.
# ----------------------------------------------------------------------------
add_custom_command(
  OUTPUT "${LUMINALSHINE_MSI_OUT_FILE}"
  DEPENDS ${_wixobjs}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LUMINALSHINE_MSI_OUT_DIR}"
  COMMAND "${LUMINALSHINE_LIGHT_EXE}" -nologo
    ${_candle_extensions}
    -ext WixUIExtension
    -b "MyScripts=${CMAKE_SOURCE_DIR}/packaging/windows/wix"
    -b "PayloadRoot=${CMAKE_BINARY_DIR}/wix_payload/"
    -out "${LUMINALSHINE_MSI_OUT_FILE}"
    ${_wixobjs}
  COMMENT "light: linking ${LUMINALSHINE_MSI_OUT_FILE}"
  VERBATIM
)

# ----------------------------------------------------------------------------
# Top-level luminalshine_msi target. Depending on the build targets
# whose outputs end up in the install / wix_payload trees forces them
# to build before staging runs (so `cmake --install` finds the
# binaries it's supposed to copy). The list mirrors the targets that
# either contribute to the install rules in cmake/packaging/windows.cmake
# or populate wix_payload from tools/CMakeLists.txt.
# ----------------------------------------------------------------------------
add_custom_target(luminalshine_msi
  DEPENDS "${LUMINALSHINE_MSI_OUT_FILE}"
)
add_dependencies(luminalshine_msi
  sunshine
  sunshinesvc
  sunshine_wgc_capture
  sunshine_display_helper
  luminalshine_xbox_bt_helper
  luminalshine_sessionmon
  dxgi-info
  audio-info
  web-ui
  build_uninstall_ui
  copy_playnite_plugin
)
