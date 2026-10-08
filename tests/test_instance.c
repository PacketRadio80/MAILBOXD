/*
 * test_instance.c — v2.8.0 single-role instance model.
 *
 * MailboxD has exactly one role (MAILBOXD_INSTANCE_MAIN). There is no
 * more Main/Secondary/Proxy switch, so this test only checks the
 * static answers instance.c now returns and that
 * mailboxd_networks_enforce_instance() is a no-op logger (it no
 * longer rewrites the caller's networks config).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/instance.h"
#include "mailboxd/networks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_int(const char *label, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", label, got, want);
        failures++;
    }
}

static void expect_str(const char *label, const char *got, const char *want)
{
    if (got == NULL || want == NULL || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got '%s' want '%s'\n", label,
                got != NULL ? got : "(null)", want != NULL ? want : "(null)");
        failures++;
    }
}

static void test_static_identity(void)
{
    expect_int("role is MAIN",
               (int)mailboxd_instance_role(), (int)MAILBOXD_INSTANCE_MAIN);
    expect_str("role name", mailboxd_instance_role_name(), "Main");
    expect_str("binary name", mailboxd_instance_binary_name(), "mailboxd");
    expect_int("offers user bbx", mailboxd_instance_offers_user_bbx(), 1);
}

static void test_standalone_is_noop(void)
{
    /* v2.8.0: standalone mode is gone; the setter/getter are stub
     * compatibility shims that never report "enabled". */
    mailboxd_instance_set_standalone(1);
    expect_int("standalone always 0 (set 1)", mailboxd_instance_standalone(), 0);
    mailboxd_instance_set_standalone(0);
    expect_int("standalone always 0 (set 0)", mailboxd_instance_standalone(), 0);
}

static void test_plugin_allowed(void)
{
    expect_int("telnet allowed", mailboxd_instance_plugin_allowed("telnet"), 1);
    expect_int("mailboxd_prterm allowed",
               mailboxd_instance_plugin_allowed("mailboxd_prterm"), 1);
    expect_int("entertain allowed", mailboxd_instance_plugin_allowed("entertain"), 1);
    expect_int("empty name blocked", mailboxd_instance_plugin_allowed(""), 0);
    expect_int("NULL name blocked", mailboxd_instance_plugin_allowed(NULL), 0);
}

static void test_enforce_is_noop(void)
{
    mailboxd_networks_config_t net;

    mailboxd_networks_config_defaults(&net);
    net.telnet = 1;

    if (mailboxd_networks_enforce_instance(&net) != MAILBOXD_OK) {
        fprintf(stderr, "FAIL enforce returned error\n");
        failures++;
        return;
    }

    /* v2.9.0: enforce_instance only logs - it must not rewrite the
     * caller's config. */
    expect_int("telnet untouched", net.telnet, 1);

    if (mailboxd_networks_enforce_instance(NULL) != MAILBOXD_ERR_INVALID) {
        fprintf(stderr, "FAIL enforce(NULL) did not return MAILBOXD_ERR_INVALID\n");
        failures++;
    }
}

int main(void)
{
    printf("instance tests role=%s binary=%s\n",
           mailboxd_instance_role_name(),
           mailboxd_instance_binary_name());

    test_static_identity();
    test_standalone_is_noop();
    test_plugin_allowed();
    test_enforce_is_noop();

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }

    puts("test_instance: ok");
    return EXIT_SUCCESS;
}
