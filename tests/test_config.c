/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_failures;

static void check_str(const char *label, const char *got, const char *want)
{
    if (got == NULL || want == NULL || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got '%s' want '%s'\n",
                label, got != NULL ? got : "(null)", want);
        g_failures++;
    }
}

int main(void)
{
    mailboxd_config_t cfg;
    const char *path = "mailboxd_test_config.ini";
    FILE *fp;

    fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "FAIL fopen\n");
        return 1;
    }

    fputs("[log]\n", fp);
    fputs("dir = logs             ; relative to install root\n", fp);
    fputs("level = info           # debug level\n", fp);
    fputs("[quoted]\n", fp);
    fputs("path = \"data;keep\" ; trailing comment\n", fp);
    fclose(fp);

    if (mailboxd_config_load(&cfg, path) != MAILBOXD_OK) {
        fprintf(stderr, "FAIL config load\n");
        remove(path);
        return 1;
    }

    check_str("inline semicolon", mailboxd_config_get(&cfg, "log", "dir", NULL),
              "logs");
    check_str("inline hash", mailboxd_config_get(&cfg, "log", "level", NULL),
              "info");
    check_str("quoted semicolon",
              mailboxd_config_get(&cfg, "quoted", "path", NULL), "data;keep");

    mailboxd_config_free(&cfg);
    remove(path);

    if (g_failures != 0) {
        fprintf(stderr, "%u failure(s)\n", g_failures);
        return 1;
    }

    puts("test_config: ok");
    return 0;
}
