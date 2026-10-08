cmake_minimum_required(VERSION 3.16)

# Default C compiler when CC / CMAKE_C_COMPILER are not set:
#   *BSD (FreeBSD, NetBSD, OpenBSD, …)  → LLVM Clang (native toolchain on BSD)
#   other → GCC
if(NOT DEFINED ENV{CC} AND NOT CMAKE_C_COMPILER)
    if(CMAKE_HOST_SYSTEM_NAME MATCHES "^(FreeBSD|OpenBSD|NetBSD|DragonFly)$")
        find_program(MAILBOXD_PREFERRED_CLANG NAMES clang)
        if(MAILBOXD_PREFERRED_CLANG)
            set(CMAKE_C_COMPILER "${MAILBOXD_PREFERRED_CLANG}"
                CACHE FILEPATH "MailboxD C compiler (Clang on BSD)" FORCE)
        else()
            find_program(MAILBOXD_PREFERRED_GCC NAMES gcc)
            if(MAILBOXD_PREFERRED_GCC)
                set(CMAKE_C_COMPILER "${MAILBOXD_PREFERRED_GCC}"
                    CACHE FILEPATH "MailboxD C compiler (GCC fallback on BSD)" FORCE)
            endif()
            unset(MAILBOXD_PREFERRED_GCC)
        endif()
        unset(MAILBOXD_PREFERRED_CLANG)
    else()
        find_program(MAILBOXD_PREFERRED_GCC NAMES gcc)
        if(MAILBOXD_PREFERRED_GCC)
            set(CMAKE_C_COMPILER "${MAILBOXD_PREFERRED_GCC}"
                CACHE FILEPATH "MailboxD primary C compiler (GCC)" FORCE)
        endif()
        unset(MAILBOXD_PREFERRED_GCC)
    endif()
endif()

project(MailboxD
    VERSION 1.0.0
    DESCRIPTION "C99 BBS/mailbox daemon (telnet + PRTERM unix-socket link)"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Export compile commands for IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CMakeDependentOption)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MAILBOXD_BUILD_DAEMON         "Build mailboxd server daemon"         ON)
option(MAILBOXD_BUILD_CLIENTS        "Build standalone CLI clients"        ON)
option(MAILBOXD_CLIENTS_ONLY         "Clients only (no daemon, no plugins)" OFF)
option(MAILBOXD_BUILD_PLUGINS      "Build transport plugins"           ON)
option(MAILBOXD_PLUGIN_TELNET      "Build TCP/IP telnet transport"     ON)
# MAILBOXD_PLUGIN_WEBSOCKET removed 2026-10-08: PRTERM is the only web
# front-end. MailboxD exposes user/operator sessions over telnet
# (PRTERM <-> MailboxD link will go over a unix-domain socket, see
# CONTINUE.md §9). External users connect via `mailboxd-telnet
# 127.0.0.1 2323` for now; a PRTERM-side telnet plugin for tcp-remote
# connections is planned but not in tree.
option(MAILBOXD_PLUGIN_ENTERTAIN   "Build Entertain plugin (chess)"          ON)
# PRTERM<->MailboxD unix-socket link (CONTINUE.md §9.4). Off by
# default — the PRTERM-side binary lands in a follow-up stage.
option(MAILBOXD_PLUGIN_MAILBOXD_PRTERM  "Build PRTERM<->MailboxD unix-socket link plugin" OFF)
option(MAILBOXD_BUILD_TESTS        "Build unit tests"                  OFF)
option(MAILBOXD_HARDENING          "Enable compiler security hardening" ON)
option(MAILBOXD_WARNINGS_AS_ERRORS "Treat warnings as errors"           OFF)
option(MAILBOXD_CRYPTO_OPENSSL     "Optional OpenSSL crypto backends"   OFF)
option(MAILBOXD_CRYPTO_LIBSODIUM   "Optional libsodium crypto backends" OFF)
option(MAILBOXD_STORAGE_SQLITE     "Optional SQLite storage backend"  ON)

cmake_dependent_option(MAILBOXD_BUILD_CLIENT_TELNET
    "Build mailboxd-telnet standalone client" ON "MAILBOXD_BUILD_CLIENTS" OFF)

if(MAILBOXD_CLIENTS_ONLY)
    set(MAILBOXD_BUILD_DAEMON OFF CACHE BOOL "Clients-only build" FORCE)
    set(MAILBOXD_BUILD_PLUGINS OFF CACHE BOOL "Clients-only build" FORCE)
    set(MAILBOXD_BUILD_CLIENTS ON CACHE BOOL "Clients-only build" FORCE)
endif()

if(NOT MAILBOXD_BUILD_DAEMON)
    set(MAILBOXD_BUILD_PLUGINS OFF CACHE BOOL "No daemon — skip plugins" FORCE)
endif()

set(MAILBOXD_HAVE_OPENSSL FALSE)
if(MAILBOXD_CRYPTO_OPENSSL)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        set(MAILBOXD_HAVE_OPENSSL TRUE)
        message(STATUS "MailboxD: OpenSSL crypto backends enabled")
    else()
        message(WARNING "MAILBOXD_CRYPTO_OPENSSL=ON but OpenSSL was not found")
    endif()
endif()

set(MAILBOXD_HAVE_LIBSODIUM FALSE)
if(MAILBOXD_CRYPTO_LIBSODIUM)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSODIUM QUIET libsodium)
    endif()
    if(LIBSODIUM_FOUND)
        set(MAILBOXD_HAVE_LIBSODIUM TRUE)
        message(STATUS "MailboxD: libsodium crypto backends enabled")
    else()
        message(WARNING "MAILBOXD_CRYPTO_LIBSODIUM=ON but libsodium was not found")
    endif()
endif()

set(MAILBOXD_HAVE_SQLITE FALSE)
if(MAILBOXD_STORAGE_SQLITE AND MAILBOXD_BUILD_DAEMON)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SQLITE3 QUIET sqlite3)
    endif()
    if(SQLITE3_FOUND)
        set(MAILBOXD_HAVE_SQLITE TRUE)
        message(STATUS "MailboxD: sqlite3 ${SQLITE3_VERSION} — SQLite storage enabled")
        if(SQLITE3_LIBRARY_DIRS)
            link_directories(${SQLITE3_LIBRARY_DIRS})
        endif()
    else()
        message(WARNING "MAILBOXD_STORAGE_SQLITE=ON but libsqlite3 was not found")
    endif()
endif()

# ---------------------------------------------------------------------------
# Platform detection (see docs/PLATFORMS.md)
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "AmigaOS")
    set(MAILBOXD_PLATFORM_AMIGAOS TRUE)
    message(STATUS "MailboxD: targeting AmigaOS 3.9+ (cross-GCC)")
elseif(WIN32)
    set(MAILBOXD_PLATFORM_WINDOWS TRUE)
    message(STATUS "MailboxD: targeting Windows 10+ (GCC/Clang via MinGW or LLVM-MinGW)")
elseif(APPLE)
    set(MAILBOXD_PLATFORM_MACOS TRUE)
    message(STATUS "MailboxD: targeting MacOS X+ (Apple Clang / LLVM Clang)")
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(MAILBOXD_PLATFORM_LINUX TRUE)
    message(STATUS "MailboxD: targeting Linux")
    add_compile_definitions(_DEFAULT_SOURCE=1)
elseif(CMAKE_SYSTEM_NAME MATCHES "FreeBSD|OpenBSD|NetBSD|DragonFly")
    set(MAILBOXD_PLATFORM_BSD TRUE)
    message(STATUS "MailboxD: targeting BSD (${CMAKE_SYSTEM_NAME})")
else()
    message(STATUS "MailboxD: targeting ${CMAKE_SYSTEM_NAME} (POSIX/GCC/Clang)")
endif()

if(NOT DEFINED MAILBOXD_PLATFORM_AMIGAOS)
    set(MAILBOXD_PLATFORM_AMIGAOS FALSE)
endif()

if(MAILBOXD_PLATFORM_AMIGAOS)
    set(MAILBOXD_PLUGIN_ENTERTAIN OFF CACHE BOOL "AmigaOS build" FORCE)
    message(STATUS "MailboxD: AmigaOS — mailboxd-telnet client (clib2); telnet plugin")
endif()

# ---------------------------------------------------------------------------
# Install layout (self-contained — all usable files in ONE tree)
#
# Default (source tree):
#   cmake -B build && cmake --build build && cmake --install build
#   → ${CMAKE_SOURCE_DIR}/bin/
#
# Alternate (explicit prefix):
#   cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
#   cmake --build build && sudo cmake --install build
#   → /usr/local/mailboxd/
#
#   cmake -B build -DCMAKE_INSTALL_PREFIX="$HOME"
#   → $HOME/mailboxd/
#
# Layout under that root:
#   mailboxd, mailboxd-start, mailboxd.ini, text/, data/, logs/, clients…
# ---------------------------------------------------------------------------

# Default prefix = source directory → install into ./bin
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}"
        CACHE PATH "MailboxD install prefix (default: source dir → ./bin)" FORCE)
endif()

get_filename_component(_mailboxd_src_abs "${CMAKE_SOURCE_DIR}" ABSOLUTE)
get_filename_component(_mailboxd_pfx_abs "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)
if(_mailboxd_pfx_abs STREQUAL _mailboxd_src_abs)
    set(MAILBOXD_INSTALL_HOME "bin")
else()
    set(MAILBOXD_INSTALL_HOME "mailboxd")
endif()
unset(_mailboxd_src_abs)
unset(_mailboxd_pfx_abs)

set(MAILBOXD_PLUGIN_INSTALL_DIR "${MAILBOXD_INSTALL_HOME}/lib/plugins")

option(MAILBOXD_INSTALL_DEV
    "Install headers, static libs, and CMake package files under the install tree"
    OFF)

message(STATUS "MailboxD install tree: ${CMAKE_INSTALL_PREFIX}/${MAILBOXD_INSTALL_HOME}"
               " (MAILBOXD_INSTALL_DEV=${MAILBOXD_INSTALL_DEV})")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Compiler.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Hardening.cmake)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(MAILBOXD_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

if(MAILBOXD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Package config (optional — MAILBOXD_INSTALL_DEV=ON)
# ---------------------------------------------------------------------------
if(MAILBOXD_INSTALL_DEV)
    include(CMakePackageConfigHelpers)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MailboxDConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/MailboxDConfig.cmake"
        INSTALL_DESTINATION "${MAILBOXD_INSTALL_HOME}/lib/cmake/MailboxD"
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/MailboxDConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/MailboxDConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/MailboxDConfigVersion.cmake"
        DESTINATION "${MAILBOXD_INSTALL_HOME}/lib/cmake/MailboxD"
    )
endif()
