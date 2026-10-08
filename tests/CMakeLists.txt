add_executable(mailboxd_test_util test_util.c)
target_link_libraries(mailboxd_test_util PRIVATE mailboxd_core)

add_executable(mailboxd_test_config test_config.c)
target_link_libraries(mailboxd_test_config PRIVATE mailboxd_core)

add_executable(mailboxd_test_storage_smoke test_storage_smoke.c)
target_link_libraries(mailboxd_test_storage_smoke PRIVATE mailboxd_core)

add_executable(mailboxd_test_instance test_instance.c)
target_link_libraries(mailboxd_test_instance PRIVATE mailboxd_core)

# test_aichat is frozen together with aichat — see plugins/entertain/.

add_test(NAME util COMMAND mailboxd_test_util)
add_test(NAME config COMMAND mailboxd_test_config)
add_test(NAME storage_smoke COMMAND mailboxd_test_storage_smoke)
add_test(NAME instance COMMAND mailboxd_test_instance)

# mailboxd_prterm plugin: unix-domain socket end-to-end test. Only
# built when the plugin itself is built (off by default).
if(MAILBOXD_PLUGIN_MAILBOXD_PRTERM)
    add_executable(mailboxd_test_mailboxd_prterm test_mailboxd_prterm.c)
    target_link_libraries(mailboxd_test_mailboxd_prterm
        PRIVATE mailboxd_core mailboxd_plugin_mailboxd_prterm)
    target_compile_definitions(mailboxd_test_mailboxd_prterm
        PRIVATE MAILBOXD_TEST_SHARE_DIR="${CMAKE_SOURCE_DIR}/share")
    add_test(NAME mailboxd_prterm COMMAND mailboxd_test_mailboxd_prterm)
    set_tests_properties(mailboxd_prterm PROPERTIES TIMEOUT 30)
endif()
