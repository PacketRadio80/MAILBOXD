# Changelog · MailboxD

All notable changes to MailboxD are documented here.

## [1.0.0]

First release. Single local BBX: mail, chat, conference, `/` commands.
RF side is entirely PRTERM's business — MailboxD never talks to a TNC,
modem or serial port. Two transports: `telnet` (CLI users) and
`mailboxd_prterm` (the PRTERM<->MailboxD unix-domain socket, see
[docs/PRTERM-LINK.md](docs/PRTERM-LINK.md)).

### Core

| Change | Detail |
|--------|--------|
| BBX core | Mail, chat, conference, `/` commands in plain-text sessions |
| Access levels | Sysop → Admin → Mod → User → Guest |
| Default Sysop | Auto-created on first start, password `PRTerm` (matches PRTERM's built-in admin default) — change it via `/passwd` |
| Transports | `telnet` (plain and crypted sessions, `:2323`), `mailboxd_prterm` (unix-domain socket, PRTERM front-end) |
| Entertain area | Chess (`/chess`, `/play`, `/mv`) as a feature plugin |
| Storage | flatfile and SQLite backends (`[storage] backend`) |
| Security | Built-in `[security]` — login brute-force bans, rate limits, CALLID bans, abuse reports |
| Clients | `mailboxd-telnet` CLI client |

### Not in this release

| Item | Note |
|------|------|
| Radio side | none — MailboxD never talks to a TNC, modem or serial port; RF is PRTERM's |
| `ssh` transport | never wired to a plugin |
| WebSocket transport | not built — the web front-end is PRTERM |
| HBX/circuit hub, inter-instance mesh (`mains_proxy`), `mailboxd-terminal` client | not built — MailboxD is a single local BBX with no inter-instance transport |
| "Main / Secondary / Proxy" multi-binary topology | not built — one `mailboxd` binary |
