add_library(mailboxd_plugin_telnet STATIC
    telnet.c
    telnet_config.c
    telnet_proto.c
)

find_package(Threads REQUIRED)

target_include_directories(mailboxd_plugin_telnet
    PRIVATE ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(mailboxd_plugin_telnet
    PRIVATE MAILBOXD_PLUGIN_BUILD
)

target_link_libraries(mailboxd_plugin_telnet PRIVATE mailboxd_core Threads::Threads)
mailboxd_link_plugin_instances(mailboxd_plugin_telnet MAILBOXD_HAVE_PLUGIN_TELNET)
mailboxd_apply_hardening(mailboxd_plugin_telnet)

if(MAILBOXD_INSTALL_DEV)
install(TARGETS mailboxd_plugin_telnet
    ARCHIVE DESTINATION ${MAILBOXD_PLUGIN_INSTALL_DIR}
    LIBRARY DESTINATION ${MAILBOXD_PLUGIN_INSTALL_DIR}
)
endif()
