# MailboxD compiler and linker hardening (MAILBOXD_HARDENING=ON)
#
# SPDX-License-Identifier: GPL-3.0-or-later

function(mailboxd_apply_hardening target)
    if(NOT MAILBOXD_HARDENING)
        return()
    endif()

    if(MAILBOXD_SUPPORTED_C_FLAGS)
        target_compile_options(${target} PRIVATE ${MAILBOXD_SUPPORTED_C_FLAGS})
    endif()

    if(MAILBOXD_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        if(NOT MAILBOXD_PLATFORM_AMIGAOS)
            target_compile_definitions(${target} PRIVATE _FORTIFY_SOURCE=2)
        endif()
    endif()

    if(NOT MAILBOXD_PLATFORM_AMIGAOS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_options(${target} PRIVATE -fPIC)
        if(MAILBOXD_COMPILER_GCC OR MAILBOXD_COMPILER_CLANG)
            target_link_options(${target} PRIVATE
                -Wl,-z,relro
                -Wl,-z,now
            )
        endif()
    endif()
endfunction()

function(mailboxd_apply_hardening_executable target)
    mailboxd_apply_hardening(${target})

    if(NOT MAILBOXD_HARDENING OR MAILBOXD_PLATFORM_AMIGAOS)
        return()
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (MAILBOXD_COMPILER_GCC OR MAILBOXD_COMPILER_CLANG))
        target_compile_options(${target} PRIVATE -fPIE)
        target_link_options(${target} PRIVATE -pie)
    endif()
endfunction()
