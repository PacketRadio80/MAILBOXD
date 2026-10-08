# Standalone CLI client (mailboxd-telnet).
# No centralized daemon; flags/env only — see src/clients/*.c for options.

set(_mailboxd_client_sources
    ${CMAKE_SOURCE_DIR}/src/core/util.c
    ${CMAKE_SOURCE_DIR}/src/core/traffic.c
    ${CMAKE_SOURCE_DIR}/src/core/terminal.c
    client_line.c
    client_display.c
)

add_library(mailboxd_client STATIC ${_mailboxd_client_sources})

target_include_directories(mailboxd_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(mailboxd_client PRIVATE MAILBOXD_CLIENT_BUILD=1)
mailboxd_apply_hardening(mailboxd_client)

if(MAILBOXD_PLATFORM_AMIGAOS)
    target_compile_options(mailboxd_client PRIVATE -mcrt=clib2)
endif()

add_library(MailboxD::client ALIAS mailboxd_client)

if(MAILBOXD_BUILD_CLIENT_TELNET)
    add_executable(mailboxd-telnet
        mailboxd_telnet.c
        ${CMAKE_SOURCE_DIR}/plugins/telnet/telnet_proto.c
    )
    target_include_directories(mailboxd-telnet PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/plugins/telnet
    )
    target_link_libraries(mailboxd-telnet PRIVATE mailboxd_client)
    mailboxd_apply_hardening_executable(mailboxd-telnet)
    if(MAILBOXD_PLATFORM_AMIGAOS)
        target_compile_options(mailboxd-telnet PRIVATE -mcrt=clib2)
        target_link_options(mailboxd-telnet PRIVATE -mcrt=clib2)
        target_link_libraries(mailboxd-telnet PRIVATE net unix)
    endif()
    install(TARGETS mailboxd-telnet RUNTIME DESTINATION ${MAILBOXD_INSTALL_HOME})
endif()

if(MAILBOXD_INSTALL_DEV)
    install(TARGETS mailboxd_client
        EXPORT MailboxDTargets
        ARCHIVE DESTINATION ${MAILBOXD_INSTALL_HOME}/lib
    )
endif()
