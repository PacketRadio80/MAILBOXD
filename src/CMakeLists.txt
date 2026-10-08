if(MAILBOXD_BUILD_DAEMON)
    add_library(mailboxd_core STATIC
        core/registry.c
        core/service.c
        core/config.c
        core/instance.c
        core/bandwidth_policy.c
        core/broadcast.c
        core/command.c
        core/command_parse.c
        core/commands_registry.c
        core/auth.c
        core/password.c
        core/crypto_config.c
        core/crypto_backends.c
        core/crypto.c
        core/traffic.c
        core/storage.c
        core/storage_flatfile.c
        core/storage_sql.c
        core/mail_sql.c
        core/session.c
        core/chat.c
        core/conference.c
        core/mail.c
        core/messages.c
        core/networks.c
        core/terminal.c
        core/texts.c
        core/util.c
        core/log.c
        core/monitor.c
        core/entertain_config.c
        core/security.c
        core/security_ban.c
        core/daemon_wrap.c
        core/privilege.c
        ${CMAKE_SOURCE_DIR}/third_party/tinysha256/tinysha256.c
        ${CMAKE_SOURCE_DIR}/third_party/monocypher/monocypher.c
        ${CMAKE_SOURCE_DIR}/third_party/tinyaes/aes.c
        ${CMAKE_SOURCE_DIR}/third_party/tinyaes/aes256gcm.c
    )

    if(MAILBOXD_HAVE_OPENSSL)
        target_sources(mailboxd_core PRIVATE core/crypto_openssl.c)
    endif()

    if(MAILBOXD_HAVE_LIBSODIUM)
        target_sources(mailboxd_core PRIVATE core/crypto_libsodium.c)
    endif()

    add_library(MailboxD::core ALIAS mailboxd_core)

    target_include_directories(mailboxd_core
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${MAILBOXD_INSTALL_HOME}/include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/core
            ${CMAKE_SOURCE_DIR}/third_party/tinysha256
            ${CMAKE_SOURCE_DIR}/third_party/monocypher
            ${CMAKE_SOURCE_DIR}/third_party/tinyaes
    )

    target_compile_definitions(mailboxd_core
        PUBLIC
            MAILBOXD_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
            MAILBOXD_VERSION_MINOR=${PROJECT_VERSION_MINOR}
            MAILBOXD_VERSION_PATCH=${PROJECT_VERSION_PATCH}
            MAILBOXD_VERSION_STRING="${PROJECT_VERSION}"
    )

    mailboxd_apply_hardening(mailboxd_core)

    find_package(Threads REQUIRED)
    target_link_libraries(mailboxd_core PUBLIC Threads::Threads)

    if(MAILBOXD_HAVE_OPENSSL)
        target_compile_definitions(mailboxd_core PRIVATE MAILBOXD_HAVE_OPENSSL)
        target_link_libraries(mailboxd_core PRIVATE OpenSSL::Crypto)
    endif()

    if(MAILBOXD_HAVE_LIBSODIUM)
        target_compile_definitions(mailboxd_core PRIVATE MAILBOXD_HAVE_LIBSODIUM)
        target_include_directories(mailboxd_core PRIVATE ${LIBSODIUM_INCLUDE_DIRS})
        target_link_directories(mailboxd_core PRIVATE ${LIBSODIUM_LIBRARY_DIRS})
        target_link_libraries(mailboxd_core PRIVATE ${LIBSODIUM_LIBRARIES})
    endif()

    if(MAILBOXD_HAVE_SQLITE)
        target_compile_definitions(mailboxd_core PRIVATE MAILBOXD_HAVE_SQLITE)
        target_include_directories(mailboxd_core PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_link_directories(mailboxd_core PRIVATE ${SQLITE3_LIBRARY_DIRS})
        target_link_libraries(mailboxd_core PRIVATE ${SQLITE3_LIBRARIES})
    endif()



    # v2.8.0: MailboxD is one binary. The "Secondary" / "Proxy" role
    # is gone — no mains_proxy, no inter-instance HBX mesh. The
    # instance_role_*.c override was what made each binary report a
    # different role; without those roles, there is no per-binary
    # source file.
    add_executable(mailboxd main.c)
    target_link_libraries(mailboxd PRIVATE mailboxd_core)
    mailboxd_apply_hardening_executable(mailboxd)

    install(TARGETS mailboxd mailboxd_core
        EXPORT MailboxDTargets
        RUNTIME DESTINATION ${MAILBOXD_INSTALL_HOME}
        ARCHIVE DESTINATION ${MAILBOXD_INSTALL_HOME}/lib
    )

    if(MAILBOXD_INSTALL_DEV)
        install(TARGETS mailboxd_core
            EXPORT MailboxDTargets
            LIBRARY DESTINATION ${MAILBOXD_INSTALL_HOME}/lib
            ARCHIVE DESTINATION ${MAILBOXD_INSTALL_HOME}/lib
        )

        install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/mailboxd
            DESTINATION ${MAILBOXD_INSTALL_HOME}/include
            FILES_MATCHING PATTERN "*.h"
        )

        install(EXPORT MailboxDTargets
            FILE MailboxDTargets.cmake
            NAMESPACE MailboxD::
            DESTINATION ${MAILBOXD_INSTALL_HOME}/lib/cmake/MailboxD
        )
    endif()

    install(FILES
        ${CMAKE_SOURCE_DIR}/share/mailboxd.ini.example
        ${CMAKE_SOURCE_DIR}/share/mailboxd-main.ini.example
        ${CMAKE_SOURCE_DIR}/share/mailboxd-standalone.ini.example
        DESTINATION ${MAILBOXD_INSTALL_HOME}
    )

    install(FILES ${CMAKE_SOURCE_DIR}/share/mailboxd-main.ini.example
        DESTINATION ${MAILBOXD_INSTALL_HOME}
        RENAME mailboxd.ini
    )

    install(FILES ${CMAKE_SOURCE_DIR}/share/commands.yaml
        ${CMAKE_SOURCE_DIR}/share/areas.yaml
        DESTINATION ${MAILBOXD_INSTALL_HOME}
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/text/
        DESTINATION ${MAILBOXD_INSTALL_HOME}/text
        FILES_MATCHING PATTERN "*.txt"
    )

    install(FILES ${CMAKE_SOURCE_DIR}/share/THIRD_PARTY_NOTICES.txt
        DESTINATION ${MAILBOXD_INSTALL_HOME}
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/share/cgi/
        DESTINATION ${MAILBOXD_INSTALL_HOME}/cgi
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )

    install(FILES ${CMAKE_SOURCE_DIR}/share/data.gitkeep
        DESTINATION ${MAILBOXD_INSTALL_HOME}/data
        RENAME .gitkeep
    )

    install(FILES ${CMAKE_SOURCE_DIR}/share/logs.gitkeep
        DESTINATION ${MAILBOXD_INSTALL_HOME}/logs
        RENAME .gitkeep
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/share/fail2ban/
        DESTINATION ${MAILBOXD_INSTALL_HOME}/fail2ban
    )

    install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/mailboxd.sh
        DESTINATION ${MAILBOXD_INSTALL_HOME}
        RENAME mailboxd-start
    )
endif()

if(MAILBOXD_BUILD_CLIENTS)
    add_subdirectory(clients)
endif()
