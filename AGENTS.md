# MailboxD — agent guide

Humans: [CONTRIBUTING.md](CONTRIBUTING.md). **v1.0.0**.

## Product

MailboxD is the mailbox and BBS (BBX) of the PRTERM stack: mail, chat,
conference, `/` commands. Standalone additional software to PRTERM. **No radio
side** — no TNC, modem or serial port code; RF is PRTERM's business.

**Text-first:** users work in plain-text terminals (`/` commands, mail, chat, line output). Core and operator docs assume text sessions on low-bandwidth links. **Extensible:** plugins and transports may add ANSI or menus without moving wire or game logic into `src/core/`. **Portable:** POSIX+ C99 shared code — Linux, *BSD, AmigaOS 3.9+, MacOS, Windows ([PLATFORMS.md](docs/PLATFORMS.md)).

> **No WebSocket, no ssh, no HBX/circuit hub, no mailboxd-terminal.**
> The "Main / Secondary / Proxy" multi-binary topology is **gone**.
> MailboxD is a single local BBX with exactly two transports: `telnet`
> (CLI users) and `mailboxd_prterm` (the PRTERM<->MailboxD unix-domain
> socket). See [PRTERM-LINK.md](docs/PRTERM-LINK.md) for the wire
> protocol.

| In tree | External |
|---------|----------|
| `src/core/` (sessions, mail, chat, commands) | PRTERM — the entire RF side; also the **web front-end** for the operator |
| `plugins/telnet` | the only user-facing TCP transport (CLI access via `mailboxd-telnet`) |
| `plugins/entertain` (chess) | Entertain Area — Main-only feature |
| `plugins/mailboxd_prterm/` (unix-domain socket) | the PRTERM<->MailboxD link — operator and user data go through here |
| Built-in `[security]` (network + abuse) | Optional external fail2ban filters in `share/fail2ban/` |

## Rules

1. Core = sessions, mail, chat, commands — no telnet wire parsing in `src/core/` (that lives in `plugins/*/…_proto.c`); the PRTERM link is its own transport, also out of core
2. **Transports are flat:** `telnet` (user sessions) and `mailboxd_prterm` (the PRTERM link) are independent — no inter-instance mesh, no circuit hub, no ssh. Do not reintroduce instance-to-instance transport code.
3. Plugins: `mailboxd_transport_plugin_t` in `plugins/`; **Entertain Area apps are plugins only** — see [ENTERTAIN.md](docs/ENTERTAIN.md)
4. Booleans: `mailboxd_parse_bool()` — `yes`/`no`
5. Buffers: [limits.h](include/mailboxd/limits.h)
6. **Portability:** POSIX+ friendly — C99 and portable POSIX in shared code; keep `*BSD` (FreeBSD, NetBSD, OpenBSD, …) and **AmigaOS 3.9+** compatible; isolate platform code (`_WIN32`, `__AMIGA__`, …). New core changes should stay easy to port.
7. **Logging:** `[log] level` = `debug` \| `stats` \| `info` \| `warn` (default). `mailboxd_log_*` writes console + daily file when enabled. See [MANUAL.md](docs/MANUAL.md) `[log]`.
8. **Docs — technical only.** Operator/INI → [MANUAL.md](docs/MANUAL.md) + `share/*.ini.example`. Commands → `share/areas.yaml` + [commands.yaml](share/commands.yaml) + [COMMANDS.md](docs/COMMANDS.md). Build → [BUILD.md](docs/BUILD.md). No planning, roadmap, or feature-status docs.
9. **Commands:** user groups are Sysop, Admin, Mod, User, Guest only (no “Staff”). Help: two lines (`/<verb> …` + `Help: …`); headers use `/help <cmd> for more`. Output: one blank line before `/command` reply, none after (not chat/mail compose). `/broadcast` (alias `/announce`) = Sysop → all online users on the local instance.
10. **Version:** current `MAILBOXD_VERSION_STRING` only in code and brief doc headers. As-is software — compact text, no upgrade/history bloat.
11. **Documentation — Linux-based.** Operator docs in `docs/`, `text/`, README, and share examples assume Linux (`HTTPD_DOCROOT`, `systemctl`, `ss`). Platform targets belong in [PLATFORMS.md](docs/PLATFORMS.md) only — each OS in its own section (Linux, AmigaOS 3.9+, *BSD [FreeBSD, NetBSD, OpenBSD, …], **MacOS X+**, **Windows 10+**; spell **MacOS**, not macOS). Do not mix MacOS into *BSD rubrics elsewhere.
12. **Git** — no commit/push unless the user asks. **Only** `ngteq <info@un1t.me>` for author/committer (`.git/config`). Push **only** via `~/.ssh/id_ed25519_ngteq`.

## Doc map

| File | Use |
|------|-----|
| [MANUAL.md](docs/MANUAL.md) | INI + operator |
| [COMMANDS.md](docs/COMMANDS.md) | Command registry, help layout |
| [commands.yaml](share/commands.yaml) | Command registry (help, aliases, access) |
| [areas.yaml](share/areas.yaml) | Menu/index area layout |
| [BUILD.md](docs/BUILD.md) | CMake |
| [DEVELOPMENT.md](docs/DEVELOPMENT.md) | Code rules |
| [TOPOLOGY.md](docs/TOPOLOGY.md) | Instance role, transports |
| [SECURITY.md](docs/SECURITY.md) | Security + spam policy |
| [PRTERM-LINK.md](docs/PRTERM-LINK.md) | PRTERM<->MailboxD wire protocol (unix socket) |
| [CLIENTS.md](docs/CLIENTS.md) | CLI clients |
| [PLATFORMS.md](docs/PLATFORMS.md) | Per-OS build notes (MacOS, Windows, …) |
| [ENTERTAIN.md](docs/ENTERTAIN.md) | Entertain Area — plugin-only apps (chess, …) |
| [LICENSING.md](docs/LICENSING.md) | License reference |

## Build

```bash
./scripts/dev-setup.sh
ctest --test-dir build   # -DMAILBOXD_BUILD_TESTS=ON
```
