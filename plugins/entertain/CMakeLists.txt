# aichat is frozen and not built — the source stays in place (aichat.c,
# entertain_aichat.h) until it is rebuilt. Set MAILBOXD_ENTERTAIN_AICHAT to 1 in
# entertain.c to bring it back.
add_library(mailboxd_plugin_entertain STATIC
    entertain.c
    chess.c
    chess_cmd.c
)

find_package(Threads REQUIRED)

target_include_directories(mailboxd_plugin_entertain
    PRIVATE ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(mailboxd_plugin_entertain
    PRIVATE MAILBOXD_PLUGIN_BUILD
)

target_link_libraries(mailboxd_plugin_entertain PRIVATE mailboxd_core Threads::Threads)
mailboxd_link_plugin_instances(mailboxd_plugin_entertain MAILBOXD_HAVE_PLUGIN_ENTERTAIN)
mailboxd_apply_hardening(mailboxd_plugin_entertain)

if(MAILBOXD_INSTALL_DEV)
install(TARGETS mailboxd_plugin_entertain
    ARCHIVE DESTINATION ${MAILBOXD_PLUGIN_INSTALL_DIR}
    LIBRARY DESTINATION ${MAILBOXD_PLUGIN_INSTALL_DIR}
)
endif()
