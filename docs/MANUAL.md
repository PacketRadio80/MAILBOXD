# Operator manual · MailboxD 1.0.0

Telnet + PRTERM-link operator reference. INI templates in `share/` — copy to
`./local/mailboxd.ini`.

> MailboxD is **a single local BBX**, one `mailboxd` binary. It has
> exactly two transports: `telnet` (CLI via `mailboxd-telnet 127.0.0.1
> 2323`) and `mailboxd_prterm` (PRTERM front-end via
> `/var/mailboxd/prterm.sock`). No ssh, no WebSocket, no HBX/circuit
> hub, no inter-instance mesh. See [PRTERM-LINK.md](PRTERM-LINK.md) for
> the wire protocol.

## Template matrix

| Template | Use |
|----------|-----|
| `mailboxd-main.ini.example` | Main install (lives alongside PRTERM) |
| `mailboxd-standalone.ini.example` | Single-host mailbox, no PRTERM |

## Role matrix

There is **one** role: **Main** (the local BBX). There is no
Secondary/Proxy, no `mains_proxy`, no circuit hub.

| Role | Behavior |
|------|----------|
| **Main** (the only role) | User BBX; `telnet` off by default (PRTERM is the web front-end), PRTERM-link unix socket on |

MailboxD has no radio side (RF is PRTERM's) — there is no AX.25 or RF
transport switch. PRTERMs INI `[mailboxd] enabled` toggles the Mailbox
tab and the PRTERM-link socket on the PRTERM side.

## Core INI sections matrix

| Section | Key keys |
|---------|----------|
| `[service]` | `name`, `prompt`, `max_online`, `nodes`, `user`, `group`, `uid`, `gid`, `login_announce` |
| `[storage]` | `backend=flatfile\|sqlite`, `path`, `user_db`, `mail_db`, `backup_path` |
| `[auth]` | `auto_login`, `guest_prefix` |
| `[traffic]` | `baud`, `line_width`, `pace_output`, `ansi`, `input_echo` |
| `[log]` | `enabled`, `dir`, `level=debug\|stats\|info\|warn` |
| `[monitor]` | `enabled`, `allow`, `follow_mailboxd`, `follow_security`, `invisible-sysop`, `invite_timeout_sec` |
| `[security]` | `enabled`, `bantime`, `findtime`, `maxretry`, `rate_limit`, `rate_window`, `ban_backend`, `ban_callid` |
| `[mail]` | `enabled`, `path`, `max_messages`, `body_max` |
| `[chat]` | `channels`, `message_max` |
| `[networks]` | `telnet` |
| `[entertain]` | `enabled` |
| `[transport.telnet]` | `bind`, `bind6`, `ipv4`, `ipv6`, `port=2323` |
| `[transport.mailboxd_prterm]` | `enabled`, `bind` (unix socket), `mode` (perms), `link_token` (shared with PRTERM) |
| `[broadcast]` | `enabled` |
| `[crypto]` | `password_hash`, `aes_gcm`, `chacha`, `x25519`, `random` |
| `[texts]` | `path` |
| `[time]` | `clock`, `clock_12h`, `am_pm`, `date`, `seconds` |

## Daemon matrix

| Item | Value |
|------|-------|
| Binaries | `mailboxd` (single binary) |
| Start | `./scripts/mailboxd.sh` or `mailboxd -c mailboxd.ini` |
| Detached | `--screen` / `--tmux` (`--attach` to join) |
| Config discovery | `-c`, then `MAILBOXD_CONFIG`, then `mailboxd.ini` next to the binary |
| First start | Default Sysop created, password `PRTerm` (matches PRTERM's built-in admin default) — change it on first login via `/passwd` |
| User/group default | `nobody`/`nogroup` (operator-editable in `mailboxd.ini` `[service]`) |

## Removed

The following are not part of MailboxD — do not plan features on top of
them:

- `ssh` transport — never wired to a plugin; gone.
- WebSocket transport — gone; the web front-end is PRTERM.
- HBX/circuit hub, `link_auth`, `mailboxd-terminal` CLI client — gone.
- `mains_proxy` (inter-instance proxy transport), `mailboxd-proxy`,
  `mailboxd-secondary` binaries — gone.
- `/proxymail`, `/proxychat`, `/proxychess` commands — gone.
- The "Standalone Main" toggle (`[instance] standalone=yes`) — kept as
  a no-op stub for compatibility, has no effect.

## Related

| Goal | Doc |
|------|-----|
| Topology | [TOPOLOGY.md](TOPOLOGY.md) |
| Security | [SECURITY.md](SECURITY.md) |
| Build | [BUILD.md](BUILD.md) |
| PRTERM<->MailboxD wire | [PRTERM-LINK.md](PRTERM-LINK.md) |
