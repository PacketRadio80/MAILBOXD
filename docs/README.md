# Documentation index · MailboxD

MailboxD is the mailbox and BBS of the PRTERM stack: mail, chat, conference and
the `/` commands. Everything radio-side belongs to PRTERM.

Start with [MANUAL.md](MANUAL.md) — it is the operator document.

## Index

| Role | Document |
|------|----------|
| Full manual | [MANUAL.md](MANUAL.md) |
| Topology | [TOPOLOGY.md](TOPOLOGY.md) |
| Commands | [COMMANDS.md](COMMANDS.md) |
| Clients | [CLIENTS.md](CLIENTS.md) |
| PRTERM<->MailboxD wire protocol | [PRTERM-LINK.md](PRTERM-LINK.md) |
| Entertain area | [ENTERTAIN.md](ENTERTAIN.md) |
| Security | [SECURITY.md](SECURITY.md) |
| Build | [BUILD.md](BUILD.md) |
| Platforms | [PLATFORMS.md](PLATFORMS.md) |
| Development | [DEVELOPMENT.md](DEVELOPMENT.md) |
| Licensing | [LICENSING.md](LICENSING.md) |

## Scope

MailboxD has **no radio side** and never talks to a TNC. RF reaches it only
through PRTERM. There is therefore no TNC, modem or RF documentation here —
that lives with PRTERM.

User access is `telnet` (plain and crypted) — and the
**PRTERM<->MailboxD link** (a unix-domain socket) is the
operator-facing transport for the web UI. A PRTERM-side telnet plugin for
tcp-remote users is planned but not in tree.
