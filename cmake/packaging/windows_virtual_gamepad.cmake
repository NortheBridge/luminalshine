# libvirtualgamepad releases are unsigned: consume only an explicitly supplied,
# signed package. Normal development builds can use an already-installed driver.
set(SUNSHINE_VHF_GAMEPAD_SIGNED_PACKAGE_DIR "" CACHE PATH "Signed libvirtualgamepad v0.1.0-beta.9 package (driver/, tools/, manifest.json)")
set(SUNSHINE_VHF_GAMEPAD_SIGNER_THUMBPRINT "" CACHE STRING "Reviewed SHA-1 certificate thumbprint signing both VHF CAT and setup tool")
set(SUNSHINE_VHF_GAMEPAD_BUNDLED 0)
if(SUNSHINE_VHF_GAMEPAD_SIGNED_PACKAGE_DIR)
    string(LENGTH "${SUNSHINE_VHF_GAMEPAD_SIGNER_THUMBPRINT}" _vhf_pin_length)
    if(NOT _vhf_pin_length EQUAL 40 OR NOT SUNSHINE_VHF_GAMEPAD_SIGNER_THUMBPRINT MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "VHF packaging requires SUNSHINE_VHF_GAMEPAD_SIGNER_THUMBPRINT (40 hexadecimal characters).")
    endif()
    find_program(_vhf_powershell NAMES powershell.exe pwsh REQUIRED)
    set(_vhf_script "${CMAKE_BINARY_DIR}/vhf-gamepad/install.ps1")
    configure_file("${CMAKE_SOURCE_DIR}/src_assets/windows/drivers/vhf-gamepad/install.ps1.in" "${_vhf_script}" @ONLY)
    execute_process(COMMAND "${_vhf_powershell}" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${_vhf_script}" -ValidateOnly -PackageRoot "${SUNSHINE_VHF_GAMEPAD_SIGNED_PACKAGE_DIR}"
        RESULT_VARIABLE _vhf_result OUTPUT_VARIABLE _vhf_output ERROR_VARIABLE _vhf_error)
    if(NOT _vhf_result EQUAL 0)
        message(FATAL_ERROR "Signed VHF package validation failed: ${_vhf_output}\n${_vhf_error}")
    endif()
    install(FILES "${_vhf_script}" "${SUNSHINE_VHF_GAMEPAD_SIGNED_PACKAGE_DIR}/manifest.json"
        DESTINATION "drivers/vhf-gamepad" COMPONENT application)
    install(FILES
        "${SUNSHINE_VHF_GAMEPAD_SIGNED_PACKAGE_DIR}/driver/VibeshineVhfGamepad.inf"
        "${SUNSHINE_VHF_GAMEPAD_SIGNED_PACKAGE_DIR}/driver/VibeshineVhfGamepad.dll"
        "${SUNSHINE_VHF_GAMEPAD_SIGNED_PACKAGE_DIR}/driver/VibeshineVhfGamepad.cat"
        DESTINATION "drivers/vhf-gamepad/driver" COMPONENT application)
    install(FILES "${SUNSHINE_VHF_GAMEPAD_SIGNED_PACKAGE_DIR}/tools/VibeshineVhfGamepadDeviceSetup.exe"
        DESTINATION "drivers/vhf-gamepad/tools" COMPONENT application)
    set(SUNSHINE_VHF_GAMEPAD_BUNDLED 1)
else()
    message(STATUS "VHF gamepad backend enabled; driver package not bundled (see docs/virtual-gamepad-driver.md).")
endif()
