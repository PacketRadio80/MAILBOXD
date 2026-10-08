# AmigaOS 3.9+ cross-compilation toolchain (bebbo-gcc / m68k-amigaos-gcc).
#
# Usage:
#   cmake -B build-amiga \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-AmigaOS.cmake \
#     -DAMIGA_SDK_PATH=/opt/amiga
#
# AMIGA_SDK_PATH may also be set via environment variable or defaults to
# /opt/amiga when that prefix exists. See docs/PLATFORMS.md.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set(CMAKE_SYSTEM_NAME AmigaOS)
set(CMAKE_SYSTEM_PROCESSOR m68k)

if(NOT AMIGA_SDK_PATH)
    if(DEFINED ENV{AMIGA_SDK_PATH} AND NOT "$ENV{AMIGA_SDK_PATH}" STREQUAL "")
        set(AMIGA_SDK_PATH "$ENV{AMIGA_SDK_PATH}")
    elseif(EXISTS "/opt/amiga/bin/m68k-amigaos-gcc")
        set(AMIGA_SDK_PATH "/opt/amiga")
    endif()
endif()

if(NOT AMIGA_SDK_PATH OR NOT EXISTS "${AMIGA_SDK_PATH}/bin/m68k-amigaos-gcc")
    message(FATAL_ERROR
        "Set AMIGA_SDK_PATH to your AmigaOS cross-compiler prefix, e.g.\n"
        "  cmake -DAMIGA_SDK_PATH=/opt/amiga -DCMAKE_TOOLCHAIN_FILE=...\n"
        "or export AMIGA_SDK_PATH=/opt/amiga")
endif()

set(AMIGA_SDK_PATH "${AMIGA_SDK_PATH}" CACHE PATH
    "AmigaOS cross-compiler prefix (bebbo-gcc)" FORCE)

set(CMAKE_C_COMPILER   "${AMIGA_SDK_PATH}/bin/m68k-amigaos-gcc")
set(CMAKE_AR           "${AMIGA_SDK_PATH}/bin/m68k-amigaos-ar")
set(CMAKE_RANLIB       "${AMIGA_SDK_PATH}/bin/m68k-amigaos-ranlib")

set(CMAKE_FIND_ROOT_PATH "${AMIGA_SDK_PATH}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
