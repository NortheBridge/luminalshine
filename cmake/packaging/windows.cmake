# windows specific packaging
install(TARGETS sunshine RUNTIME DESTINATION "." COMPONENT application)

# Hardening: include zlib1.dll (loaded via LoadLibrary() in openssl's libcrypto.a)
install(FILES "${ZLIB}" DESTINATION "." COMPONENT application)

if(WEBRTC_RUNTIME_DLL)
    install(FILES "${WEBRTC_RUNTIME_DLL}" DESTINATION "." COMPONENT application)
endif()

# ARM64: include minhook-detours DLL (shared library for ARM64)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64" AND DEFINED _MINHOOK_DLL)
    install(FILES "${_MINHOOK_DLL}" DESTINATION "." COMPONENT application)
endif()

# Bundle msys2 ucrt64 runtime DLLs that the binaries link against dynamically.
#
# Two distinct sets need to ship:
#
#   1. ICU/iconv — msys2 only provides import libraries (.dll.a) for these, so
#      the linker always emits dynamic references to libicuin*.dll, libicudt*.dll,
#      libicuuc*.dll, and libiconv-*.dll regardless of -static.
#
#   2. The GCC/C++ runtime (libstdc++-6.dll, libgcc_s_seh-1.dll, libwinpthread-1.dll,
#      libssp-0.dll, etc.). The link line passes -static and lists libstdc++.a /
#      libwinpthread.a / libssp.a explicitly, but in practice transitive dependencies
#      pulled in by Boost, libcurl, FFmpeg, et al. can still drag in the dynamic
#      runtime — and a single dynamic reference is enough for the loader to demand
#      the .dll at process start. Bundling them unconditionally is cheap and
#      removes the foot-gun: end-users hit "the code execution cannot proceed
#      because libstdc++-6.dll was not found" otherwise.
#
# DLL discovery uses MINGW_PREFIX (set by msys2 shells) with a CI-runner
# fallback. Globs avoid hardcoding ABI version numbers (ICU 78 today, libstdc++-6
# today, etc.) so the same code keeps working as msys2 bumps versions.
if(DEFINED ENV{MINGW_PREFIX} AND IS_DIRECTORY "$ENV{MINGW_PREFIX}/bin")
    set(_msys2_bin_dir "$ENV{MINGW_PREFIX}/bin")
elseif(IS_DIRECTORY "D:/a/_temp/msys64/ucrt64/bin")
    set(_msys2_bin_dir "D:/a/_temp/msys64/ucrt64/bin")
elseif(IS_DIRECTORY "/ucrt64/bin")
    set(_msys2_bin_dir "/ucrt64/bin")
else()
    set(_msys2_bin_dir "")
endif()

if(_msys2_bin_dir)
    # ICU + iconv: always required, hard-fail if missing (something is wrong with the toolchain).
    file(GLOB _msys2_link_dlls
        "${_msys2_bin_dir}/libicudt*.dll"
        "${_msys2_bin_dir}/libicuin*.dll"
        "${_msys2_bin_dir}/libicuuc*.dll"
        "${_msys2_bin_dir}/libiconv*.dll"
    )
    if(NOT _msys2_link_dlls)
        message(FATAL_ERROR
                "Could not locate ICU/iconv runtime DLLs in ${_msys2_bin_dir}.\n"
                "  Without these the installed sunshine.exe will fail to launch with\n"
                "  'libicuin*.dll was not found'. Check that the msys2 ucrt64 packages\n"
                "  mingw-w64-ucrt-x86_64-icu and -libiconv are installed.")
    endif()

    # GCC/C++ runtime: bundle every runtime DLL that may be dynamically pulled in
    # by us or any transitive dependency. Each pattern is best-effort — missing
    # ones are simply not shipped — but we hard-fail if libstdc++ is missing
    # because that's the one we know is required.
    file(GLOB _msys2_gcc_runtime_dlls
        "${_msys2_bin_dir}/libstdc++*.dll"
        "${_msys2_bin_dir}/libgcc_s*.dll"
        "${_msys2_bin_dir}/libwinpthread*.dll"
        "${_msys2_bin_dir}/libssp*.dll"
        "${_msys2_bin_dir}/libatomic*.dll"
        "${_msys2_bin_dir}/libgomp*.dll"
        "${_msys2_bin_dir}/libquadmath*.dll"
    )
    set(_has_libstdcxx FALSE)
    foreach(_dll IN LISTS _msys2_gcc_runtime_dlls)
        get_filename_component(_dll_name "${_dll}" NAME)
        if(_dll_name MATCHES "^libstdc\\+\\+")
            set(_has_libstdcxx TRUE)
            break()
        endif()
    endforeach()
    if(NOT _has_libstdcxx)
        message(FATAL_ERROR
                "Could not locate libstdc++-*.dll in ${_msys2_bin_dir}.\n"
                "  Without it the installed sunshine.exe will fail to launch with\n"
                "  'libstdc++-6.dll was not found'. Check that the msys2 ucrt64 package\n"
                "  mingw-w64-ucrt-x86_64-gcc-libs is installed.")
    endif()

    set(_msys2_runtime_dlls ${_msys2_link_dlls} ${_msys2_gcc_runtime_dlls})
    message(STATUS "Bundling msys2 runtime DLLs from ${_msys2_bin_dir}:")
    foreach(_dll IN LISTS _msys2_runtime_dlls)
        get_filename_component(_dll_name "${_dll}" NAME)
        message(STATUS "  - ${_dll_name}")
    endforeach()
    install(FILES ${_msys2_runtime_dlls} DESTINATION "." COMPONENT application)
else()
    message(FATAL_ERROR
            "Could not determine msys2 bin directory for runtime DLL bundling.\n"
            "  Set MINGW_PREFIX in the environment (msys2 shells do this automatically).")
endif()

# ViGEmBus installer is no longer bundled or managed by the installer

# Adding tools
install(TARGETS dxgi-info RUNTIME DESTINATION "tools" COMPONENT dxgi)
install(TARGETS audio-info RUNTIME DESTINATION "tools" COMPONENT audio)


# Helpers and tools
# - Playnite launcher helper used for Playnite-managed app launches
# - WGC capture helper used by the WGC display backend
# - Display helper used for applying/reverting display settings
if (TARGET playnite-launcher)
    install(TARGETS playnite-launcher RUNTIME DESTINATION "tools" COMPONENT application)
endif()
if (TARGET sunshine_wgc_capture)
    install(TARGETS sunshine_wgc_capture RUNTIME DESTINATION "tools" COMPONENT application)
endif()
if (TARGET sunshine_display_helper)
    install(TARGETS sunshine_display_helper RUNTIME DESTINATION "tools" COMPONENT application)
endif()
install(FILES "${CMAKE_BINARY_DIR}/uninstall.exe" DESTINATION "." COMPONENT application)

# Drivers (SudoVDA virtual display)
set(SUDOVDA_SOURCE_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/sudovda")
set(SUDOVDA_DRIVER_FILES
    "${SUDOVDA_SOURCE_DIR}/install.ps1"
    "${SUDOVDA_SOURCE_DIR}/uninstall.bat"
    "${SUDOVDA_SOURCE_DIR}/SudoVDA.inf"
    "${SUDOVDA_SOURCE_DIR}/SudoVDA.dll"
    "${SUDOVDA_SOURCE_DIR}/sudovda.cat"
    "${SUDOVDA_SOURCE_DIR}/sudovda.cer"
    "${SUDOVDA_SOURCE_DIR}/nefconc.exe"
)

foreach(_sudovda_file IN LISTS SUDOVDA_DRIVER_FILES)
    if (NOT EXISTS "${_sudovda_file}")
        message(FATAL_ERROR "Required SudoVDA driver artifact missing: ${_sudovda_file}")
    endif()
    file(SIZE "${_sudovda_file}" _sudovda_file_size)
    if (_sudovda_file_size EQUAL 0)
        message(FATAL_ERROR "Required SudoVDA driver artifact is empty (0 bytes): ${_sudovda_file}")
    endif()
endforeach()
unset(_sudovda_file_size)

install(FILES ${SUDOVDA_DRIVER_FILES}
        DESTINATION "drivers/sudovda"
        COMPONENT sudovda)

# MTT Virtual Display Driver — primary backend on modern Windows builds.
# Source binaries are vendored under third-party/mtt-vdd/ (see that
# directory's README for upstream and signing details). The install script
# lives alongside SudoVDA's under src_assets so package metadata is
# maintained in one place.
set(MTT_VDD_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/mtt-vdd")
set(MTT_VDD_INSTALL_SCRIPT "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/vdd/install.ps1")
set(MTT_VDD_DRIVER_FILES
    "${MTT_VDD_INSTALL_SCRIPT}"
    "${MTT_VDD_SOURCE_DIR}/MttVDD.inf"
    "${MTT_VDD_SOURCE_DIR}/MttVDD.dll"
    "${MTT_VDD_SOURCE_DIR}/mttvdd.cat"
    "${MTT_VDD_SOURCE_DIR}/vdd_settings.xml.template"
    "${MTT_VDD_SOURCE_DIR}/LICENSE"
)

foreach(_mttvdd_file IN LISTS MTT_VDD_DRIVER_FILES)
    if (NOT EXISTS "${_mttvdd_file}")
        message(FATAL_ERROR "Required MTT VDD artifact missing: ${_mttvdd_file}")
    endif()
    file(SIZE "${_mttvdd_file}" _mttvdd_file_size)
    if (_mttvdd_file_size EQUAL 0)
        message(FATAL_ERROR "Required MTT VDD artifact is empty (0 bytes): ${_mttvdd_file}")
    endif()
endforeach()
unset(_mttvdd_file_size)

install(FILES ${MTT_VDD_DRIVER_FILES}
        DESTINATION "drivers/vdd"
        COMPONENT mttvdd)

# Mandatory scripts
install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/sunshine-setup.ps1"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/service/"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/migration/"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/path/"
        DESTINATION "scripts"
        COMPONENT assets)

# Configurable options for the service
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/autostart/"
        DESTINATION "scripts"
        COMPONENT autostart)

# scripts
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/firewall/"
        DESTINATION "scripts"
        COMPONENT firewall)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/gamepad/"
        DESTINATION "scripts"
        COMPONENT assets)

# Sunshine assets
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/"
        DESTINATION "${SUNSHINE_ASSETS_DIR}"
        COMPONENT assets)

# Plugins (copy plugin folders such as `plugins/playnite` into the package)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/plugins/"
        DESTINATION "plugins"
        COMPONENT assets)

# copy assets (excluding shaders) to build directory, for running without install
file(COPY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/"
        DESTINATION "${CMAKE_BINARY_DIR}/assets"
        PATTERN "shaders" EXCLUDE)

if(WEBRTC_RUNTIME_DLL)
    file(COPY "${WEBRTC_RUNTIME_DLL}"
            DESTINATION "${CMAKE_BINARY_DIR}")
endif()
# use junction for shaders directory
cmake_path(CONVERT "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/shaders"
        TO_NATIVE_PATH_LIST shaders_in_build_src_native)
cmake_path(CONVERT "${CMAKE_BINARY_DIR}/assets/shaders" TO_NATIVE_PATH_LIST shaders_in_build_dest_native)
if(NOT EXISTS "${CMAKE_BINARY_DIR}/assets/shaders")
    execute_process(COMMAND cmd.exe /c mklink /J "${shaders_in_build_dest_native}" "${shaders_in_build_src_native}")
endif()

set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}\\\\sunshine.ico")

# The name of the directory that will be created in C:/Program files/
# Keep install directory as Sunshine regardless of displayed product name
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Sunshine")

# Setting components groups and dependencies
set(CPACK_COMPONENT_GROUP_CORE_EXPANDED true)

# sunshine binary
set(CPACK_COMPONENT_APPLICATION_DISPLAY_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_COMPONENT_APPLICATION_DESCRIPTION "${CMAKE_PROJECT_NAME} main application and required components.")
set(CPACK_COMPONENT_APPLICATION_GROUP "Core")
set(CPACK_COMPONENT_APPLICATION_REQUIRED true)
set(CPACK_COMPONENT_APPLICATION_DEPENDS assets)

# service auto-start script
set(CPACK_COMPONENT_AUTOSTART_DISPLAY_NAME "Launch on Startup")
set(CPACK_COMPONENT_AUTOSTART_DESCRIPTION "If enabled, launches LuminalShine automatically on system startup.")
set(CPACK_COMPONENT_AUTOSTART_GROUP "Core")

# assets
set(CPACK_COMPONENT_ASSETS_DISPLAY_NAME "Required Assets")
set(CPACK_COMPONENT_ASSETS_DESCRIPTION "Shaders, default box art, and web UI.")
set(CPACK_COMPONENT_ASSETS_GROUP "Core")
set(CPACK_COMPONENT_ASSETS_REQUIRED true)

# drivers
# MTT Virtual Display Driver — primary backend, installed by default.
set(CPACK_COMPONENT_MTTVDD_DISPLAY_NAME "Virtual Display Driver (MTT)")
set(CPACK_COMPONENT_MTTVDD_DESCRIPTION "MikeTheTech's IDD-based virtual display driver. Default backend for per-client virtual displays. Recommended for current and Insider Windows builds.")
set(CPACK_COMPONENT_MTTVDD_GROUP "Drivers")
set(CPACK_COMPONENT_MTTVDD_REQUIRED true)

# SudoVDA — kept available as a compatibility/legacy backend. Not installed
# by default on new installs; users on Insider builds with MTT incompatibility
# can choose 'Modify' in Add/Remove Programs to add it.
set(CPACK_COMPONENT_SUDOVDA_DISPLAY_NAME "SudoVDA (Compatibility)")
set(CPACK_COMPONENT_SUDOVDA_DESCRIPTION "Legacy virtual display driver. Optional fallback for older Windows builds or troubleshooting MTT VDD.")
set(CPACK_COMPONENT_SUDOVDA_GROUP "Drivers")
set(CPACK_COMPONENT_SUDOVDA_DISABLED true)

# audio tool
set(CPACK_COMPONENT_AUDIO_DISPLAY_NAME "audio-info")
set(CPACK_COMPONENT_AUDIO_DESCRIPTION "CLI tool providing information about sound devices.")
set(CPACK_COMPONENT_AUDIO_GROUP "Tools")

# display tool
set(CPACK_COMPONENT_DXGI_DISPLAY_NAME "dxgi-info")
set(CPACK_COMPONENT_DXGI_DESCRIPTION "CLI tool providing information about graphics cards and displays.")
set(CPACK_COMPONENT_DXGI_GROUP "Tools")

# firewall scripts
set(CPACK_COMPONENT_FIREWALL_DISPLAY_NAME "Add Firewall Exclusions")
set(CPACK_COMPONENT_FIREWALL_DESCRIPTION "Scripts to enable or disable firewall rules.")
set(CPACK_COMPONENT_FIREWALL_GROUP "Scripts")

# gamepad scripts are bundled under assets and not exposed as a separate component

# include specific packaging (WiX only)
include(${CMAKE_MODULE_PATH}/packaging/windows_wix.cmake)
