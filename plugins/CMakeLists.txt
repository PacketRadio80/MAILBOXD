# Link a transport plugin into the MailboxD binary (single local BBX,
# v2.8.0 — there is no mailboxd-secondary/mailboxd-proxy anymore).
function(mailboxd_link_plugin_instances plugin_target define_name)
    if(TARGET mailboxd)
        target_link_libraries(mailboxd PRIVATE ${plugin_target})
        if(NOT "${define_name}" STREQUAL "")
            target_compile_definitions(mailboxd PRIVATE ${define_name})
        endif()
    endif()
endfunction()

if(MAILBOXD_PLUGIN_TELNET)
    add_subdirectory(telnet)
endif()

# WebSocket plugin removed 2026-10-08 — see CMakeLists.txt for the rationale.

if(MAILBOXD_PLUGIN_ENTERTAIN)
    add_subdirectory(entertain)
endif()

# PRTERM<->MailboxD link plugin (unix-domain socket). Off by default.
if(MAILBOXD_PLUGIN_MAILBOXD_PRTERM)
    add_subdirectory(mailboxd_prterm)
endif()
