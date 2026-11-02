include("${VALUES}")
if(SERVER_BUILD STREQUAL "")
    # UTC, once for this build; CMake also honors SOURCE_DATE_EPOCH for reproducible builds.
    string(TIMESTAMP SERVER_BUILD "%Y-%m-%dT%H:%M:%SZ" UTC)
endif()
set(_content "// Generated build metadata. Do not edit.\n#pragma once\nnamespace server_metadata {\ninline constexpr int release = ${SERVER_RELEASE};\ninline constexpr char build[] = \"${SERVER_BUILD}\";\n}\n")
# Preserve mtime when CI regenerates with the same stamp after signing.
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" _previous)
    if(_previous STREQUAL _content)
        return()
    endif()
endif()
file(WRITE "${OUTPUT}" "${_content}")
