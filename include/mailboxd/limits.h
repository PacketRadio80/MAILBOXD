/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_LIMITS_H
#define MAILBOXD_LIMITS_H

#include "mailboxd/traffic.h"

/** INI parser line buffer size. */
#define MAILBOXD_CONFIG_LINE_MAX 512

/** Maximum path length for files and directories. */
#define MAILBOXD_PATH_MAX 512

/** Relative paths under the MailboxD install root (see @ref MAILBOXD_ENV_ROOT). */
#define MAILBOXD_DIR_DATA "data"
#define MAILBOXD_DIR_TEXT "text"
#define MAILBOXD_DIR_LOGS "logs"
#define MAILBOXD_FILE_CONFIG "mailboxd.ini"
#define MAILBOXD_FILE_COMMANDS "commands.yaml"
#define MAILBOXD_FILE_AREAS    "areas.yaml"
#define MAILBOXD_DAEMON_BINARY "mailboxd"

/** Command registry (share/commands.yaml + share/areas.yaml). */
#define MAILBOXD_COMMANDS_MAX             64u
#define MAILBOXD_AREAS_MAX                12u
#define MAILBOXD_AREAS_SUB_MAX             4u
#define MAILBOXD_COMMANDS_VERBS_PER_GROUP 16u
#define MAILBOXD_COMMANDS_ALIASES_MAX     48u
#define MAILBOXD_COMMANDS_ALIAS_PER      8u
#define MAILBOXD_COMMANDS_ALIAS_LINES    8u
#define MAILBOXD_MENU_LEVELS_MAX           5u
#define MAILBOXD_MENU_AREAS_PER_LEVEL     12u
#define MAILBOXD_RIGHTS_TARGET_RULES_MAX  16u
#define MAILBOXD_RIGHTS_PROMOTE_RULES_MAX  8u
#define MAILBOXD_RIGHTS_DEMOTE_RULES_MAX   4u
#define MAILBOXD_COMMANDS_HELP_LINE_MAX  96u
#define MAILBOXD_COMMANDS_HEADER_MAX     128u

/** Environment variable: absolute path to the MailboxD install directory. */
#define MAILBOXD_ENV_ROOT "MAILBOXD_ROOT"

#define MAILBOXD_CONFIG_SECTION_MAX 128
#define MAILBOXD_CONFIG_KEY_MAX 128
#define MAILBOXD_CONFIG_VALUE_MAX 256

/** Maximum tokens per MailboxD command line (verb + args). */
#define MAILBOXD_CMD_TOKEN_MAX 16

#define MAILBOXD_CMD_VERB_MAX 32

/** Maximum registered transport plugins. */
#define MAILBOXD_MAX_PLUGINS 16

/** Maximum length of the global session prompt (empty = no visible prompt). */
#define MAILBOXD_PROMPT_MAX 32

/** Maximum single allocation for duplicated strings (bytes). */
#define MAILBOXD_ALLOC_MAX (256u * 1024u)

/** Default maximum simultaneous online sessions (guests included). */
#define MAILBOXD_DEFAULT_MAX_ONLINE 35u

/** Default guest session lifetime before auto-disconnect (minutes). */
#define MAILBOXD_DEFAULT_GUEST_TIMEOUT_MINUTES 30u

/** Command history depth for interactive line editors. */
#define MAILBOXD_HISTORY_MAX 25u

/** SQLite fallback copy suffix (see `[storage] backup_path`). */
#define MAILBOXD_STORAGE_BACKUP_SUFFIX ".flb"

/** Default seconds between SQLite DB fallback copies. */
#define MAILBOXD_STORAGE_BACKUP_INTERVAL_DEFAULT_SEC 300u

/** [security] defaults — short cool-down bans (seconds). */
#define MAILBOXD_SECURITY_DEFAULT_MAXRETRY 5u
#define MAILBOXD_SECURITY_DEFAULT_FINDTIME_SEC 600u
#define MAILBOXD_SECURITY_DEFAULT_BANTIME_SEC 600u
/** Abuse (excessive spam/flood) — higher bar than login failures; no ban for normal use. */
#define MAILBOXD_SECURITY_DEFAULT_ABUSE_MAXRETRY 30u
#define MAILBOXD_SECURITY_DEFAULT_ABUSE_FINDTIME_SEC 600u
#define MAILBOXD_SECURITY_DEFAULT_RATE_LIMIT 30u
#define MAILBOXD_SECURITY_DEFAULT_RATE_WINDOW_SEC 60u
#define MAILBOXD_SECURITY_BAN_MAX 256u
#define MAILBOXD_SECURITY_TRACK_MAX 512u

/** AX.25 CALL-SSID or HBX link_id (same ban table). */
#define MAILBOXD_CALLID_MAX 64u

#endif /* MAILBOXD_LIMITS_H */
