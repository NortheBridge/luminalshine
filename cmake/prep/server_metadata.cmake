# Additive /serverinfo metadata. Never infer release status from a local tag.
set(SUNSHINE_SERVER_RELEASE "0" CACHE STRING "Server release class: 0 dev, 1 stable, 2 prerelease")
set(SUNSHINE_SERVER_BUILD "" CACHE STRING "UTC build-start timestamp override (empty generates at build time)")
foreach(_metadata_var SUNSHINE_SERVER_RELEASE SUNSHINE_SERVER_BUILD)
    if(DEFINED ENV{${_metadata_var}})
        set(${_metadata_var} "$ENV{${_metadata_var}}" CACHE STRING "Server metadata override" FORCE)
    endif()
endforeach()
if(NOT SUNSHINE_SERVER_RELEASE MATCHES "^[012]$")
    message(FATAL_ERROR "SUNSHINE_SERVER_RELEASE must be 0, 1 or 2")
endif()
if(NOT SUNSHINE_SERVER_BUILD STREQUAL "" AND
   NOT SUNSHINE_SERVER_BUILD MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z$")
    message(FATAL_ERROR "SUNSHINE_SERVER_BUILD must be UTC ISO 8601: YYYY-MM-DDTHH:MM:SSZ")
endif()
set(_metadata_dir "${CMAKE_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_LIST_DIR}/server_metadata_values.cmake.in"
    "${_metadata_dir}/server_metadata_values.cmake" @ONLY)
# Run once per build invocation, including option-only or dependency-only rebuilds.
# A fixed CI stamp writes no changes, so packaging cannot invalidate signed binaries.
add_custom_target(server_metadata
    BYPRODUCTS "${_metadata_dir}/server_metadata.h"
    COMMAND "${CMAKE_COMMAND}" "-DVALUES=${_metadata_dir}/server_metadata_values.cmake"
        "-DOUTPUT=${_metadata_dir}/server_metadata.h"
        -P "${CMAKE_CURRENT_LIST_DIR}/write_server_metadata.cmake"
    DEPENDS "${_metadata_dir}/server_metadata_values.cmake"
        "${CMAKE_CURRENT_LIST_DIR}/write_server_metadata.cmake"
    COMMENT "Generating server build metadata"
    VERBATIM)
list(APPEND SUNSHINE_TARGET_DEPENDENCIES server_metadata)
include_directories("${_metadata_dir}")
