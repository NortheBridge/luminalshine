cmake_minimum_required(VERSION 3.20)

project(sunshine_tools)

include_directories(
        "${CMAKE_SOURCE_DIR}"
)

set(TOOL_SOURCES
        "${CMAKE_SOURCE_DIR}/src/logging.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp"
)

add_executable(dxgi-info dxgi.cpp ${TOOL_SOURCES})
# Use OUTPUT_NAME rather than renaming the CMake target so existing
# install(TARGETS dxgi-info ...) / target_link_libraries() / etc. references
# elsewhere in the build graph keep working. The on-disk binary becomes
# `luminalshine-dxgi-info.exe`, which namespaces a generic-sounding tool
# name away from anything else a user might have in their PATH.
set_target_properties(dxgi-info PROPERTIES CXX_STANDARD 23
        OUTPUT_NAME luminalshine-dxgi-info)
target_link_libraries(dxgi-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        dxgi
        ${PLATFORM_LIBRARIES}
)
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp ${TOOL_SOURCES})
set_target_properties(audio-info PROPERTIES CXX_STANDARD 23
        OUTPUT_NAME luminalshine-audio-info)
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ksuser
        ${PLATFORM_LIBRARIES}
)
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(sunshinesvc sunshinesvc.cpp)
set_target_properties(sunshinesvc PROPERTIES CXX_STANDARD 23
        OUTPUT_NAME luminalshinesvc)
target_link_libraries(sunshinesvc
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# WiX consumes the staged copy via `!(bindpath.PayloadRoot)tools\luminalshinesvc.exe`
# (see packaging/windows/wix/custom_actions.wxs). $<TARGET_FILE:...> resolves
# to the OUTPUT_NAME-renamed path, so this single step keeps WiX and CMake
# in lockstep.
add_custom_command(TARGET sunshinesvc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/wix_payload/tools"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:sunshinesvc>" "${CMAKE_BINARY_DIR}/wix_payload/tools/luminalshinesvc.exe"
)

# Playnite launcher: per-launch helper that hosts a private IPC server
add_executable(playnite-launcher
        playnite_launcher/main.cpp
        playnite_launcher/launcher.cpp
        playnite_launcher/arguments.cpp
        playnite_launcher/cleanup.cpp
        playnite_launcher/focus_utils.cpp
        playnite_launcher/lossless_scaling.cpp
        playnite_launcher/playnite_process.cpp)
set_target_properties(playnite-launcher PROPERTIES CXX_STANDARD 23
        OUTPUT_NAME luminalshine-playnite-launcher)
set_target_properties(playnite-launcher PROPERTIES WIN32_EXECUTABLE TRUE)
target_include_directories(playnite-launcher PRIVATE ${CMAKE_SOURCE_DIR}/tools/playnite_launcher)
target_sources(playnite-launcher PRIVATE
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_ipc.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_protocol.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp)
target_compile_definitions(playnite-launcher PRIVATE SUNSHINE_PLAYNITE_LAUNCHER)
target_link_libraries(playnite-launcher
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        nlohmann_json::nlohmann_json
        user32
        userenv
        ole32
        oleaut32
        Psapi
        shell32
        shlwapi
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(playnite-launcher PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
# Add sunshine_wgc_capture executable
add_executable(sunshine_wgc_capture
        sunshine_wgc_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp)
set_target_properties(sunshine_wgc_capture PROPERTIES CXX_STANDARD 23
        OUTPUT_NAME luminalshine_wgc_capture)

# Define helper build flag to conditionally compile logging
target_compile_definitions(sunshine_wgc_capture PRIVATE
        SUNSHINE_EXTERNAL_PROCESS=1
)

target_link_libraries(sunshine_wgc_capture
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        d3d11
        dxgi
        ole32
        user32
        gdi32
        windowsapp
        avrt
        shcore
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshine_wgc_capture PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Display settings helper: detached helper that applies/reverts display settings via IPC
add_executable(sunshine_display_helper
        display_settings_helper.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp)
set_target_properties(sunshine_display_helper PROPERTIES CXX_STANDARD 23
        OUTPUT_NAME luminalshine_display_helper)
target_compile_definitions(sunshine_display_helper PRIVATE SETUP_LIBDISPLAYDEVICE_LOGGING)
target_include_directories(sunshine_display_helper PRIVATE ${CMAKE_SOURCE_DIR}/third-party)
target_link_libraries(sunshine_display_helper
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        libdisplaydevice::display_device
        user32
        wtsapi32
        advapi32
        secur32
        ole32
        taskschd
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshine_display_helper PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Xbox Bluetooth reconnect helper: independent Windows service that nudges
# Steam's XInput hotplug path when a Bluetooth Xbox controller re-arrives.
# Registered with SCM separately from LuminalShineService (see
# packaging/windows/wix/custom_actions.wxs) so a regression in this helper
# cannot affect streaming. Disabled by default; opt-in via
# %ProgramData%\LuminalShine\config\luminalshine_xboxcontroller_state.json.
#
# Pulls in ViGEmClient.cpp directly (the same source the main sunshine
# binary compiles) -- the third-party suppressions for that file are
# declared at root scope in cmake/targets/common.cmake and the tools
# subdirectory is included in that DIRECTORY list so the flags propagate.
add_executable(luminalshine_xbox_bt_helper
        luminalshine_xbox_bt_helper.cpp
        ${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp)
set_target_properties(luminalshine_xbox_bt_helper PROPERTIES CXX_STANDARD 23
        OUTPUT_NAME luminalshine_xbox_bt_helper)
target_include_directories(luminalshine_xbox_bt_helper PRIVATE
        ${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include)
target_link_libraries(luminalshine_xbox_bt_helper
        ${CMAKE_THREAD_LIBS_INIT}
        nlohmann_json::nlohmann_json
        advapi32
        setupapi
        shell32
        ole32
        ${PLATFORM_LIBRARIES})
target_compile_options(luminalshine_xbox_bt_helper PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# WiX consumes the staged copy via `!(bindpath.PayloadRoot)tools\luminalshine_xbox_bt_helper.exe`
# (see packaging/windows/wix/custom_actions.wxs), so mirror the post-build
# step that sunshinesvc uses to keep WiX and CMake in lockstep.
add_custom_command(TARGET luminalshine_xbox_bt_helper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/wix_payload/tools"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:luminalshine_xbox_bt_helper>" "${CMAKE_BINARY_DIR}/wix_payload/tools/luminalshine_xbox_bt_helper.exe"
)
