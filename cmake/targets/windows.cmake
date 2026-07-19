# windows specific target definitions
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)

# The LuminalVGD FFI staticlib must exist before the sunshine link step.
add_dependencies(sunshine luminal_vgd_ffi_build)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        $<TARGET_OBJECTS:sunshine_rc_object>
        Windowsapp.lib
        Wtsapi32.lib
        avrt.lib
        Mscms.lib
        version.lib)

# Copy Playnite plugin sources into build output (for packaging/installers)
## Copy Playnite plugin sources into build output (for packaging/installers)
## Make the copy step incremental: only re-run when source files change.
file(GLOB_RECURSE SUNSHINE_PLAYNITE_PLUGIN_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/plugins/playnite/*")
set(SUNSHINE_PLAYNITE_PLUGIN_STAMP "${CMAKE_BINARY_DIR}/plugins/playnite/.copy_stamp")

add_custom_command(
        OUTPUT ${SUNSHINE_PLAYNITE_PLUGIN_STAMP}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/plugins/playnite"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/plugins/playnite" "${CMAKE_BINARY_DIR}/plugins/playnite"
        COMMAND ${CMAKE_COMMAND} -E touch ${SUNSHINE_PLAYNITE_PLUGIN_STAMP}
        DEPENDS ${SUNSHINE_PLAYNITE_PLUGIN_SOURCES}
        COMMENT "Copying Playnite plugin sources"
)
add_custom_target(copy_playnite_plugin DEPENDS ${SUNSHINE_PLAYNITE_PLUGIN_STAMP})
add_dependencies(sunshine copy_playnite_plugin)

# Ensure the Windows display helper is built and placed next to the Sunshine binary
# so the runtime launcher can find it reliably.
if (TARGET sunshine_display_helper)
    # Build helper before sunshine to make the copy step reliable
    add_dependencies(sunshine sunshine_display_helper)

    # Copy helper next to the sunshine executable after build
    add_custom_command(TARGET sunshine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:sunshine_display_helper>
                $<TARGET_FILE_DIR:sunshine>
        COMMENT "Copying sunshine_display_helper next to Sunshine binary")
endif()

# The Xbox Bluetooth helper is an SCM-managed service, not a process spawned
# by the main host -- Windows starts it directly from its registered path
# under `<install>\tools\`. So no "copy next to sunshine.exe" step is needed;
# the wix_payload copy in tools/CMakeLists.txt is sufficient. We only need
# to make sure the binary exists by the time package_msi runs, which we get
# by chaining the dependency through the sunshine target.
if (TARGET luminalshine_xbox_bt_helper)
    add_dependencies(sunshine luminalshine_xbox_bt_helper)
endif()

# Enable libdisplaydevice logging in the main Sunshine binary only
target_compile_definitions(sunshine PRIVATE SUNSHINE_USE_DISPLAYDEVICE_LOGGING)

# Build lightweight uninstall UI executable (same UX as installer, no embedded MSI payload)
set(SUNSHINE_UNINSTALL_UI_EXE "${CMAKE_BINARY_DIR}/uninstall.exe")
add_custom_command(
    OUTPUT "${SUNSHINE_UNINSTALL_UI_EXE}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/packaging/windows/bootstrapper/build_bootstrapper.ps1" -BuildDir "${CMAKE_BINARY_DIR}" -UninstallOnly -OutputName "uninstall.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/cpack_artifacts/uninstall.exe" "${SUNSHINE_UNINSTALL_UI_EXE}"
    DEPENDS "${CMAKE_SOURCE_DIR}/packaging/windows/bootstrapper/build_bootstrapper.ps1"
            "${CMAKE_SOURCE_DIR}/packaging/windows/bootstrapper/LuminalShineInstaller.cs"
            "${CMAKE_SOURCE_DIR}/packaging/windows/bootstrapper/app.manifest"
            "${CMAKE_SOURCE_DIR}/LICENSE"
            "${CMAKE_SOURCE_DIR}/sunshine.ico"
    COMMENT "Building lightweight LuminalShine uninstaller UI"
)
add_custom_target(build_uninstall_ui ALL DEPENDS "${SUNSHINE_UNINSTALL_UI_EXE}")

# Convenience target — preserves the historical `package_msi` name
# that CI and dev workflows already invoke. As of PR2.c of the WiX 7
# migration this aliases the new hand-authored luminalshine_msi
# target in cmake/packaging/windows_wix.cmake (which drives candle.exe
# + light.exe directly, replacing the CPACK_GENERATOR "WIX" path).
add_custom_target(package_msi
    DEPENDS luminalshine_msi
    COMMENT "Building MSI installer via hand-authored WiX 3 pipeline"
)

# Build custom elevated installer EXE that wraps the generated MSI
add_custom_target(package_installer
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/packaging/windows/bootstrapper/build_bootstrapper.ps1" -BuildDir "${CMAKE_BINARY_DIR}"
    DEPENDS package_msi
    COMMENT "Building custom installer executable"
)
