/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include "mailboxd/daemon_wrap.h"
#include "mailboxd/limits.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void mailboxd_daemon_launch_opts_defaults(mailboxd_daemon_launch_opts_t *opts)
{
    if (opts == NULL) {
        return;
    }

    memset(opts, 0, sizeof(*opts));
    mailboxd_strlcpy(opts->session, MAILBOXD_DAEMON_DEFAULT_SESSION,
                  sizeof(opts->session));
}

static int path_executable(const char *path)
{
    return path != NULL && path[0] != '\0' && access(path, X_OK) == 0;
}

static int find_in_path(const char *name, char *out, size_t out_len)
{
    const char *path_env;
    char copy[MAILBOXD_PATH_MAX];
    char *save = NULL;
    char *dir;

    if (name == NULL || out == NULL || out_len == 0) {
        return 0;
    }

    path_env = getenv("PATH");
    if (path_env == NULL || path_env[0] == '\0') {
        return 0;
    }

    snprintf(copy, sizeof(copy), "%s", path_env);
    for (dir = strtok_r(copy, ":", &save); dir != NULL;
         dir = strtok_r(NULL, ":", &save)) {
        if (mailboxd_path_join(out, out_len, dir, name) != MAILBOXD_OK) {
            continue;
        }
        if (path_executable(out)) {
            return 1;
        }
    }

    return 0;
}

static int wrapped_child(void)
{
    const char *env = getenv(MAILBOXD_DAEMON_WRAP_ENV);

    return env != NULL && env[0] == '1';
}

static int in_screen(void)
{
    const char *sty = getenv("STY");

    return sty != NULL && sty[0] != '\0';
}

static int in_tmux(void)
{
    const char *tmux = getenv("TMUX");

    return tmux != NULL && tmux[0] != '\0';
}

static int arg_is_launch_flag(const char *arg)
{
    if (arg == NULL) {
        return 0;
    }

    return strcmp(arg, "-f") == 0 || strcmp(arg, "--foreground") == 0 ||
           strcmp(arg, "--attach") == 0 || strcmp(arg, "--screen") == 0 ||
           strcmp(arg, "--tmux") == 0;
}

static int build_inner_argv(char **out, int out_max,
                            const char *binary,
                            int argc, char **argv)
{
    int outc = 0;
    int i;

    if (out == NULL || out_max < 2 || binary == NULL) {
        return -1;
    }

    out[outc++] = (char *)binary;

    if (!wrapped_child()) {
        out[outc++] = "-f";
    }

    for (i = 1; i < argc && outc < out_max - 1; i++) {
        if (strcmp(argv[i], "--screen") == 0 || strcmp(argv[i], "--tmux") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-' &&
                !arg_is_launch_flag(argv[i + 1])) {
                i++;
            }
            continue;
        }
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0 ||
            strcmp(argv[i], "--attach") == 0) {
            continue;
        }

        out[outc++] = argv[i];
    }

    out[outc] = NULL;
    return outc;
}

static mailboxd_result_t exec_screen_attach(const char *session)
{
    char screen_path[MAILBOXD_PATH_MAX];
    char *args[8];
    int n = 0;

    if (!find_in_path("screen", screen_path, sizeof(screen_path))) {
        fprintf(stderr, "mailboxd: GNU screen not found in PATH\n");
        return MAILBOXD_ERR_IO;
    }

    args[n++] = screen_path;
    args[n++] = "-r";
    args[n++] = (char *)session;
    args[n++] = NULL;

    execv(screen_path, args);
    fprintf(stderr, "mailboxd: screen attach failed\n");
    return MAILBOXD_ERR_IO;
}

static mailboxd_result_t exec_tmux_attach(const char *session)
{
    char tmux_path[MAILBOXD_PATH_MAX];
    char *args[8];
    int n = 0;

    if (!find_in_path("tmux", tmux_path, sizeof(tmux_path))) {
        fprintf(stderr, "mailboxd: tmux not found in PATH\n");
        return MAILBOXD_ERR_IO;
    }

    args[n++] = tmux_path;
    args[n++] = "attach";
    args[n++] = "-t";
    args[n++] = (char *)session;
    args[n++] = NULL;

    execv(tmux_path, args);
    fprintf(stderr, "mailboxd: tmux attach failed\n");
    return MAILBOXD_ERR_IO;
}

static mailboxd_result_t spawn_screen(const char *session, const char *binary,
                                   int argc, char **argv)
{
    char screen_path[MAILBOXD_PATH_MAX];
    char *inner[MAILBOXD_CMD_TOKEN_MAX + 4];
    char wrap_env[64];
    int innerc;
    char *args[MAILBOXD_CMD_TOKEN_MAX + 8];
    int n = 0;

    if (!find_in_path("screen", screen_path, sizeof(screen_path))) {
        fprintf(stderr, "mailboxd: GNU screen not found in PATH\n");
        return MAILBOXD_ERR_IO;
    }

    innerc = build_inner_argv(inner, (int)(sizeof(inner) / sizeof(inner[0])),
                              binary, argc, argv);
    if (innerc < 0) {
        return MAILBOXD_ERR_INVALID;
    }

    snprintf(wrap_env, sizeof(wrap_env), "%s=1", MAILBOXD_DAEMON_WRAP_ENV);

    args[n++] = screen_path;
    args[n++] = "-dmS";
    args[n++] = (char *)session;
    args[n++] = "env";
    args[n++] = wrap_env;
    {
        int i;

        for (i = 0; i < innerc; i++) {
            args[n++] = inner[i];
        }
    }
    args[n++] = NULL;

    execv(screen_path, args);
    fprintf(stderr, "mailboxd: screen spawn failed\n");
    return MAILBOXD_ERR_IO;
}

static mailboxd_result_t spawn_tmux(const char *session, const char *binary,
                                 int argc, char **argv)
{
    char tmux_path[MAILBOXD_PATH_MAX];
    char *inner[MAILBOXD_CMD_TOKEN_MAX + 4];
    char wrap_env[64];
    int innerc;
    char *args[MAILBOXD_CMD_TOKEN_MAX + 8];
    int n = 0;

    if (!find_in_path("tmux", tmux_path, sizeof(tmux_path))) {
        fprintf(stderr, "mailboxd: tmux not found in PATH\n");
        return MAILBOXD_ERR_IO;
    }

    innerc = build_inner_argv(inner, (int)(sizeof(inner) / sizeof(inner[0])),
                              binary, argc, argv);
    if (innerc < 0) {
        return MAILBOXD_ERR_INVALID;
    }

    snprintf(wrap_env, sizeof(wrap_env), "%s=1", MAILBOXD_DAEMON_WRAP_ENV);

    args[n++] = tmux_path;
    args[n++] = "new-session";
    args[n++] = "-d";
    args[n++] = "-s";
    args[n++] = (char *)session;
    args[n++] = "env";
    args[n++] = wrap_env;
    {
        int i;

        for (i = 0; i < innerc; i++) {
            args[n++] = inner[i];
        }
    }
    args[n++] = NULL;

    execv(tmux_path, args);
    fprintf(stderr, "mailboxd: tmux spawn failed\n");
    return MAILBOXD_ERR_IO;
}

mailboxd_result_t mailboxd_daemon_apply_launch_opts(const mailboxd_daemon_launch_opts_t *opts,
                                              const char *binary_path,
                                              int argc, char **argv)
{
    const char *binary = binary_path;

    if (opts == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (binary == NULL || binary[0] == '\0') {
        binary = MAILBOXD_DAEMON_BINARY;
    }

    if (opts->use_screen && opts->use_tmux) {
        fprintf(stderr, "mailboxd: use --screen or --tmux, not both\n");
        return MAILBOXD_ERR_INVALID;
    }

    if (opts->attach) {
        if (opts->use_screen) {
            return exec_screen_attach(opts->session);
        }
        if (opts->use_tmux) {
            return exec_tmux_attach(opts->session);
        }

        fprintf(stderr, "mailboxd: --attach requires --screen or --tmux\n");
        return MAILBOXD_ERR_INVALID;
    }

    if (wrapped_child() || opts->foreground) {
        return MAILBOXD_OK;
    }

    if (opts->use_screen) {
        if (in_screen()) {
            return MAILBOXD_OK;
        }

        if (spawn_screen(opts->session, binary, argc, argv) != MAILBOXD_OK) {
            return MAILBOXD_ERR_IO;
        }
        printf("mailboxd: started in screen session '%s'\n", opts->session);
        printf("attach: screen -r %s\n", opts->session);
        exit(EXIT_SUCCESS);
    }

    if (opts->use_tmux) {
        if (in_tmux()) {
            return MAILBOXD_OK;
        }

        if (spawn_tmux(opts->session, binary, argc, argv) != MAILBOXD_OK) {
            return MAILBOXD_ERR_IO;
        }
        printf("mailboxd: started in tmux session '%s'\n", opts->session);
        printf("attach: tmux attach -t %s\n", opts->session);
        exit(EXIT_SUCCESS);
    }

    return MAILBOXD_OK;
}
