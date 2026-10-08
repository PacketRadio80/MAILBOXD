/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include "mailboxd/privilege.h"
#include "mailboxd/config.h"
#include "mailboxd/log.h"
#include "mailboxd/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)

static int parse_optional_uid(const char *raw, uid_t *out)
{
    char *end;
    unsigned long value;

    if (raw == NULL || raw[0] == '\0') {
        return 0;
    }
    errno = 0;
    value = strtoul(raw, &end, 10);
    if (end == raw || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (uid_t)value;
    return 1;
}

static int parse_optional_gid(const char *raw, gid_t *out)
{
    char *end;
    unsigned long value;

    if (raw == NULL || raw[0] == '\0') {
        return 0;
    }
    errno = 0;
    value = strtoul(raw, &end, 10);
    if (end == raw || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (gid_t)value;
    return 1;
}

static mailboxd_result_t resolve_target(
    const mailboxd_config_t *config,
    uid_t *uid_out,
    gid_t *gid_out,
    char name_buf[64],
    size_t name_buf_len)
{
    const char *user_name;
    const char *group_name;
    const char *uid_raw;
    const char *gid_raw;
    struct passwd *pw = NULL;
    struct group *gr = NULL;
    uid_t uid_override = 0;
    gid_t gid_override = 0;
    int have_uid = 0;
    int have_gid = 0;

    user_name = mailboxd_config_get(config, "service", "user", NULL);
    group_name = mailboxd_config_get(config, "service", "group", NULL);
    uid_raw = mailboxd_config_get(config, "service", "uid", NULL);
    gid_raw = mailboxd_config_get(config, "service", "gid", NULL);

    have_uid = parse_optional_uid(uid_raw, &uid_override);
    if (have_uid < 0) {
        mailboxd_log_warn("[service] invalid uid=%s", uid_raw);
        return MAILBOXD_ERR_INVALID;
    }
    have_gid = parse_optional_gid(gid_raw, &gid_override);
    if (have_gid < 0) {
        mailboxd_log_warn("[service] invalid gid=%s", gid_raw);
        return MAILBOXD_ERR_INVALID;
    }

    if (user_name != NULL && user_name[0] != '\0') {
        pw = getpwnam(user_name);
        if (pw == NULL) {
            mailboxd_log_warn("[service] unknown user=%s", user_name);
            return MAILBOXD_ERR_INVALID;
        }
        *uid_out = pw->pw_uid;
        *gid_out = pw->pw_gid;
        if (name_buf_len > 0) {
            snprintf(name_buf, name_buf_len, "%s", pw->pw_name);
        }
    } else if (have_uid) {
        pw = getpwuid(uid_override);
        if (pw == NULL) {
            mailboxd_log_warn("[service] unknown uid=%s", uid_raw);
            return MAILBOXD_ERR_INVALID;
        }
        *uid_out = pw->pw_uid;
        *gid_out = pw->pw_gid;
        if (name_buf_len > 0) {
            snprintf(name_buf, name_buf_len, "%s", pw->pw_name);
        }
    } else {
        return MAILBOXD_ERR_INVALID;
    }

    if (have_uid) {
        *uid_out = uid_override;
    }
    if (group_name != NULL && group_name[0] != '\0') {
        gr = getgrnam(group_name);
        if (gr == NULL) {
            mailboxd_log_warn("[service] unknown group=%s", group_name);
            return MAILBOXD_ERR_INVALID;
        }
        *gid_out = gr->gr_gid;
    } else if (have_gid) {
        if (getgrgid(gid_override) == NULL) {
            mailboxd_log_warn("[service] unknown gid=%s", gid_raw);
            return MAILBOXD_ERR_INVALID;
        }
        *gid_out = gid_override;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t drop_to(uid_t uid, gid_t gid, const char *name)
{
    if (getuid() != 0) {
        mailboxd_log_warn("[service] privilege drop requires root start (euid=0)");
        return MAILBOXD_ERR_DENIED;
    }

    if (name != NULL && name[0] != '\0') {
        if (initgroups(name, gid) != 0) {
            mailboxd_log_warn("[service] initgroups(%s) failed: %s",
                            name, strerror(errno));
            return MAILBOXD_ERR_IO;
        }
    }

    if (setgid(gid) != 0) {
        mailboxd_log_warn("[service] setgid(%u) failed: %s",
                        (unsigned)gid, strerror(errno));
        return MAILBOXD_ERR_IO;
    }

    if (setuid(uid) != 0) {
        mailboxd_log_warn("[service] setuid(%u) failed: %s",
                        (unsigned)uid, strerror(errno));
        return MAILBOXD_ERR_IO;
    }

    if (setuid(0) == 0 || seteuid(0) == 0) {
        mailboxd_log_warn("[service] privilege drop incomplete — still root");
        return MAILBOXD_ERR_IO;
    }

    mailboxd_log_info("[service] dropped privileges to uid=%u gid=%u (%s)",
                   (unsigned)uid, (unsigned)gid,
                   name != NULL && name[0] != '\0' ? name : "?");
    return MAILBOXD_OK;
}

#endif /* !_WIN32 */

mailboxd_result_t mailboxd_privilege_apply_from_config(const mailboxd_config_t *config)
{
#if defined(_WIN32)
    (void)config;
    return MAILBOXD_OK;
#else
    const char *user_name;
    uid_t target_uid = 0;
    gid_t target_gid = 0;
    char resolved_name[64];
    mailboxd_result_t rc;

    if (config == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    resolved_name[0] = '\0';
    user_name = mailboxd_config_get(config, "service", "user", NULL);

    if (geteuid() == 0) {
        if ((user_name == NULL || user_name[0] == '\0') &&
            mailboxd_config_get(config, "service", "uid", NULL) == NULL) {
            mailboxd_log_warn(
                "[service] refusing to run as root — set [service] user= "
                "(and optional group=)");
            return MAILBOXD_ERR_DENIED;
        }

        rc = resolve_target(config, &target_uid, &target_gid,
                            resolved_name, sizeof(resolved_name));
        if (rc != MAILBOXD_OK) {
            return rc;
        }

        return drop_to(target_uid, target_gid,
                       resolved_name[0] != '\0' ? resolved_name : user_name);
    }

    if (user_name != NULL && user_name[0] != '\0') {
        struct passwd *pw = getpwnam(user_name);

        if (pw == NULL) {
            mailboxd_log_warn("[service] unknown configured user=%s", user_name);
            return MAILBOXD_OK;
        }
        if ((uid_t)geteuid() != pw->pw_uid) {
            mailboxd_log_warn(
                "[service] running as uid=%u but [service] user=%s (uid=%u)",
                (unsigned)geteuid(), user_name, (unsigned)pw->pw_uid);
        }
    }

    return MAILBOXD_OK;
#endif
}
