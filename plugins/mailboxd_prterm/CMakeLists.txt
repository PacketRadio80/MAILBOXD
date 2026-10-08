# mailboxd_prterm — PRTERM <-> MailboxD unix-socket link.
#
# Built only when explicitly enabled. Off by default; the prterm-side
# binary prterm-mailboxd is not in tree yet (Stufe 2 of CONTINUE.md §9).

add_library(mailboxd_plugin_mailboxd_prterm STATIC
    mailboxd_prterm.c
    mailboxd_prterm_config.c
)

target_include_directories(mailboxd_plugin_mailboxd_prterm
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(mailboxd_plugin_mailboxd_prterm
    PUBLIC mailboxd_core
)

mailboxd_apply_hardening(mailboxd_plugin_mailboxd_prterm)

# Compile-time option: enable/disable. Default is OFF — the plugin is
# a feature for installations that have a PRTERM host.
option(MAILBOXD_PLUGIN_MAILBOXD_PRTERM
    "Build PRTERM<->MailboxD unix-socket link plugin" OFF)

if(MAILBOXD_PLUGIN_MAILBOXD_PRTERM)
    target_compile_definitions(mailboxd_plugin_mailboxd_prterm
        PRIVATE MAILBOXD_HAVE_PLUGIN_MAILBOXD_PRTERM)

    # Define is set both on the plugin (so it can `#ifdef` itself
    # in source) and on the binaries (so main.c and the registry
    # can see it). mailboxd_link_plugin_instances sets it on the
    # binaries; we mirror the same for the plugin target.
    mailboxd_link_plugin_instances(mailboxd_plugin_mailboxd_prterm
                                  MAILBOXD_HAVE_PLUGIN_MAILBOXD_PRTERM)
endif()
