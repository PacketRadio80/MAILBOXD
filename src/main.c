/*
 * MailboxD 2.8.0 — single local BBX daemon.
 *
 * The historical "Main / Secondary / Proxy" multi-binary topology is
 * gone (see docs/CHANGELOG.md): one `mailboxd` binary, one local user
 * BBX, one PRTERM<->MailboxD link over a unix-domain socket.
 *
 * Config: -c path/to/mailboxd.ini
 * Plugins: CMake MAILBOXD_PLUGIN_* (telnet, mailboxd_prterm, ...).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include "mailboxd/mailboxd.h"
#include "mailboxd/instance.h"
#include "mailboxd/service.h"
#include "mailboxd/beacon.h"
#include "mailboxd/registry.h"
#include "mailboxd/config.h"
#include "mailboxd/command.h"
#include "mailboxd/commands_registry.h"
#include "mailboxd/daemon_wrap.h"
#include "mailboxd/util.h"
#include "mailboxd/limits.h"
#include "mailboxd/privilege.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <libgen.h>

#ifdef MAILBOXD_HAVE_PLUGIN_TELNET
extern const mailboxd_transport_plugin_t mailboxd_plugin_telnet;
#endif
#ifdef MAILBOXD_HAVE_PLUGIN_ENTERTAIN
extern const mailboxd_transport_plugin_t mailboxd_plugin_entertain;
#endif
#ifdef MAILBOXD_HAVE_PLUGIN_MAILBOXD_PRTERM
extern const mailboxd_transport_plugin_t mailboxd_plugin_mailboxd_prterm;
#endif

static void register_builtin_plugins(void)
{
    /* Register every compiled plugin; instance/INI gating is at apply time. */
#ifdef MAILBOXD_HAVE_PLUGIN_TELNET
    mailboxd_registry_register(&mailboxd_plugin_telnet);
#endif
#ifdef MAILBOXD_HAVE_PLUGIN_ENTERTAIN
    mailboxd_registry_register(&mailboxd_plugin_entertain);
#endif
#ifdef MAILBOXD_HAVE_PLUGIN_MAILBOXD_PRTERM
    mailboxd_registry_register(&mailboxd_plugin_mailboxd_prterm);
#endif
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "MailboxD %s — %s instance (%s)\n"
            "\n"
            "Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  -c, --config <file>  Load INI configuration file\n"
            "  -f, --foreground     Run in foreground (default)\n"
            "  --screen [name]       Run detached in GNU screen (default: %s)\n"
            "  --tmux [name]         Run detached in tmux (default: %s)\n"
            "  --attach              Attach to screen/tmux session (with --screen/--tmux)\n"
            "  -h, --help           Show this help\n"
            "  -V, --version        Show version\n"
            "\n"
            "Instance: mailboxd (single local BBX, v2.8.0)\n"
            "\n",
            MAILBOXD_VERSION_STRING,
            mailboxd_instance_role_name(),
            mailboxd_instance_binary_name(),
            prog,
            mailboxd_instance_binary_name(),
            mailboxd_instance_binary_name());
}

static void print_version(void)
{
    printf("MailboxD %s — %s (%s)%s\n",
           MAILBOXD_VERSION_STRING,
           mailboxd_instance_role_name(),
           mailboxd_instance_binary_name(),
           mailboxd_instance_offers_user_bbx() ? "" : " [no user BBX]");
}

static void list_plugins_cb(const mailboxd_transport_plugin_t *plugin, void *userdata)
{
    (void)userdata;
    printf("  - %s (kind=%d, version=%u)\n",
           plugin->name, (int)plugin->kind, plugin->version);
}

static int path_is_readable(const char *path)
{
    return path != NULL && path[0] != '\0' && access(path, R_OK) == 0;
}

/**
 * When -c is omitted: MAILBOXD_CONFIG, then <binary>/mailboxd.ini (install layout).
 */
static const char *discover_config_path(char *buf, size_t buflen,
                                        const char *argv0)
{
    const char *env;

    if (buf == NULL || buflen == 0) {
        return NULL;
    }

    env = getenv("MAILBOXD_CONFIG");
    if (path_is_readable(env)) {
        snprintf(buf, buflen, "%s", env);
        return buf;
    }

    if (argv0 == NULL || argv0[0] == '\0') {
        return NULL;
    }

#if defined(__linux__)
    {
        char exe[MAILBOXD_PATH_MAX];
        char etc[MAILBOXD_PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

        if (n > 0) {
            char *slash;
            size_t dir_len;

            exe[n] = '\0';
            slash = strrchr(exe, '/');
            if (slash != NULL) {
                dir_len = (size_t)(slash - exe);
                if (dir_len + 1 < sizeof(etc)) {
                    memcpy(etc, exe, dir_len);
                    etc[dir_len] = '\0';
                    if (mailboxd_path_join(buf, buflen, etc, MAILBOXD_FILE_CONFIG) ==
                            MAILBOXD_OK &&
                        path_is_readable(buf)) {
                        return buf;
                    }
                }
            }
        }
    }
#endif

    snprintf(buf, buflen, "%s", argv0);
    {
        char path_copy[MAILBOXD_PATH_MAX];
        char candidate[MAILBOXD_PATH_MAX];
        char *dir;

        snprintf(path_copy, sizeof(path_copy), "%s", argv0);
        dir = dirname(path_copy);
        if (mailboxd_path_join(candidate, sizeof(candidate), dir,
                            MAILBOXD_FILE_CONFIG) == MAILBOXD_OK &&
            path_is_readable(candidate)) {
            snprintf(buf, buflen, "%s", candidate);
            return buf;
        }
    }

    return NULL;
}

static void setup_install_root(const char *config_path)
{
    const char *env;

    env = getenv(MAILBOXD_ENV_ROOT);
    if (env != NULL && env[0] != '\0') {
        mailboxd_install_root_set(env);
        return;
    }

    if (config_path != NULL && config_path[0] != '\0') {
        char dir[MAILBOXD_PATH_MAX];

        if (mailboxd_path_dirname(config_path, dir, sizeof(dir)) == MAILBOXD_OK) {
            mailboxd_install_root_set(dir);
        }
    }
}


/* ── Signal handling ────────────────────────────────────────── */

static volatile sig_atomic_t g_signal_received;

static void signal_handler(int sig)
{
    g_signal_received = sig;
    /* SIGPIPE: just ignore — send() with MSG_NOSIGNAL handles most cases */
    if (sig == SIGPIPE) {
        return;
    }
    /* SIGTERM/SIGINT: wake mailboxd_service_run()'s loop so it actually
     * returns - g_signal_received alone is only checked after the loop
     * exits, to print which signal caused the shutdown. */
    mailboxd_service_request_stop_async();
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

    /* SIGPIPE: ignore — prevents silent death on broken socket writes */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);

    /* SIGTERM: graceful shutdown */
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);

    /* SIGINT: graceful shutdown (Ctrl+C) */
    sigaction(SIGINT, &sa, NULL);

    g_signal_received = 0;
}

int main(int argc, char *argv[])
{
    mailboxd_service_t *service;
    mailboxd_config_t config;
    const char *config_path = NULL;
    char discovered_config[MAILBOXD_PATH_MAX];
    mailboxd_daemon_launch_opts_t launch;
    int have_config = 0;
    int i;
    mailboxd_result_t rc;

    mailboxd_daemon_launch_opts_defaults(&launch);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0) {
            launch.foreground = 1;
            continue;
        }
        if (strcmp(argv[i], "--attach") == 0) {
            launch.attach = 1;
            continue;
        }
        if (strcmp(argv[i], "--screen") == 0) {
            launch.use_screen = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-' &&
                strcmp(argv[i + 1], "-c") != 0 &&
                strcmp(argv[i + 1], "--config") != 0) {
                mailboxd_strlcpy(launch.session, argv[++i], sizeof(launch.session));
            }
            continue;
        }
        if (strcmp(argv[i], "--tmux") == 0) {
            launch.use_tmux = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-' &&
                strcmp(argv[i + 1], "-c") != 0 &&
                strcmp(argv[i + 1], "--config") != 0) {
                mailboxd_strlcpy(launch.session, argv[++i], sizeof(launch.session));
            }
            continue;
        }
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for %s\n", argv[i]);
                return EXIT_FAILURE;
            }
            config_path = argv[++i];
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    rc = mailboxd_daemon_apply_launch_opts(&launch, argv[0], argc, argv);
    if (rc != MAILBOXD_OK) {
        return EXIT_FAILURE;
    }

    if (config_path == NULL) {
        config_path = discover_config_path(discovered_config,
                                           sizeof(discovered_config), argv[0]);
    }

    register_builtin_plugins();

    printf("MailboxD %s starting (%s / %s)\n",
           MAILBOXD_VERSION_STRING,
           mailboxd_instance_binary_name(),
           mailboxd_instance_role_name());

    service = mailboxd_service_create(NULL);
    if (service == NULL) {
        fprintf(stderr, "Failed to create service (out of memory?)\n");
        return EXIT_FAILURE;
    }

    if (config_path != NULL) {
        rc = mailboxd_config_load(&config, config_path);

        if (rc != MAILBOXD_OK) {
            fprintf(stderr, "Failed to load config '%s'\n", config_path);
            mailboxd_service_destroy(service);
            return EXIT_FAILURE;
        }

        have_config = 1;
        printf("Loaded configuration: %s\n", config_path);

        rc = mailboxd_privilege_apply_from_config(&config);
        if (rc != MAILBOXD_OK) {
            fprintf(stderr,
                    "Privilege setup failed — %s\n",
                    mailboxd_result_name(rc));
            mailboxd_config_free(&config);
            mailboxd_service_destroy(service);
            return EXIT_FAILURE;
        }

        setup_install_root(config_path);

        rc = mailboxd_commands_registry_init();
        if (rc != MAILBOXD_OK) {
            fprintf(stderr, "Failed to load command registry\n");
            mailboxd_config_free(&config);
            mailboxd_service_destroy(service);
            return EXIT_FAILURE;
        }

        rc = mailboxd_service_apply_config(service, &config, config_path);
        if (rc != MAILBOXD_OK) {
            fprintf(stderr,
                    "Failed to apply configuration (storage path writable? "
                    "telnet bind ok?) — %s\n",
                    mailboxd_result_name(rc));
            mailboxd_commands_registry_shutdown();
            mailboxd_config_free(&config);
            mailboxd_service_destroy(service);
            return EXIT_FAILURE;
        }

        printf("Available transport plugins:\n");
        mailboxd_registry_foreach(list_plugins_cb, NULL);

        /* Start the beacon daemon if configured. */
        if (mailboxd_beacon_start(&config) != MAILBOXD_OK) {
            fprintf(stderr, "Warning: beacon daemon failed to start\n");
        }
    } else {
        fprintf(stderr,
                "No configuration loaded. Use -c <mailboxd.ini>, set MAILBOXD_CONFIG, "
                "or run mailboxd-start.\n");
        fprintf(stderr, "No transports will listen without a config file.\n");
    }

    mailboxd_service_set_launch_binary(service, argv[0]);

    install_signal_handlers();

    printf("MailboxD %s running (%s). Signals: SIGPIPE=ignored, SIGTERM/SIGINT=graceful stop.\n",
           MAILBOXD_VERSION_STRING, mailboxd_instance_binary_name());

    mailboxd_service_run(service);

    if (g_signal_received) {
        printf("MailboxD: received signal %d (%s) — shutting down.\n",
               g_signal_received,
               g_signal_received == SIGTERM ? "SIGTERM" :
               g_signal_received == SIGINT  ? "SIGINT"  : "unknown");
    } else {
        printf("MailboxD: service loop exited normally.\n");
    }

    if (mailboxd_service_shutdown_mode(service) == MAILBOXD_SHUTDOWN_RESTART) {
        mailboxd_service_restart_exec(service);
    }

    if (have_config) {
        mailboxd_config_free(&config);
    }
    mailboxd_beacon_stop();
    mailboxd_commands_registry_shutdown();
    mailboxd_service_destroy(service);

    printf("MailboxD: shutdown complete (exit %d, signal=%d).\n",
           EXIT_SUCCESS, (int)g_signal_received);
    return EXIT_SUCCESS;
}
