# Topology · MailboxD 1.0.0

MailboxD has **no radio side** — no TNC, modem or serial port. RF is entirely
PRTERM's business.

> The web front-end is **PRTERM**, and the link between PRTERM and MailboxD
> is a unix-domain socket under `/var/mailboxd/prterm.sock`. External users
> connect via `mailboxd-telnet 127.0.0.1 2323` (the telnet transport is kept
> for this). A PRTERM-side telnet plugin for tcp-remote users is planned but
> not in tree. There is no ssh transport and no HBX/circuit hub — MailboxD
> is a single local BBX with no inter-instance mesh.

## Role matrix

There is **one** role:

| Role | Binary | User transports | Notes |
|------|--------|-----------------|-------|
| **Main** (the only role) | `mailboxd` | `telnet` (CLI) and `mailboxd_prterm` (PRTERM<->MailboxD link) | Single local BBX; no Secondary, no Proxy, no circuit hub |

## PRTERM<->MailboxD link matrix

| Item | Value |
|------|-------|
| Path | `/var/mailboxd/prterm.sock` (default; `[transport.mailboxd_prterm] bind=…`) |
| Perms | `0660` (default; `[transport.mailboxd_prterm] mode=…`) |
| Auth | `[transport.mailboxd_prterm] link_token` shared secret; PRTERM has it in `[mailboxd] link_token` |
| Scope | Sole PRTERM<->MailboxD transport; user/operator traffic is **not** on this socket |
| Wire | See [PRTERM-LINK.md](PRTERM-LINK.md) |

## Broadcast matrix

| Command | Scope | Level | Band-state precondition |
|---------|-------|-------|-------------------------|
| `/broadcast <msg>` | Local online users | Sysop | PRTERM reports `band_free_seconds ≥ 150` (last 2:30 of silence) |

## Related

| Goal | Doc |
|------|-----|
| Manual | [MANUAL.md](MANUAL.md) |
| Security | [SECURITY.md](SECURITY.md) |
| PRTERM<->MailboxD wire | [PRTERM-LINK.md](PRTERM-LINK.md) |
