# Clients · MailboxD 1.0.0

User-facing connection clients for MailboxD sessions.

> There is no WebSocket transport. The web browser talks to PRTERM, and
> PRTERM talks to MailboxD over a unix-domain socket. See
> [PRTERM-LINK.md](PRTERM-LINK.md) for the wire protocol and
> [MANUAL.md](MANUAL.md) for the `[transport.mailboxd_prterm]` INI section.

## Client matrix

| Client | Transport | Default port | Notes |
|--------|-----------|--------------|-------|
| `mailboxd-telnet` | TCP telnet | 2323 | External users (CLI) |
| `PRTERM` (web) | Unix-domain socket `/var/mailboxd/prterm.sock` | — | The web front-end. PRTERM is **the** way most users connect. |

## Session matrix

| Item | Value |
|------|-------|
| Wire format | Plain text lines + `/` commands (telnet), framed byte stream (PRTERM<->MailboxD) |
| ANSI | `[traffic] ansi=yes` optional (telnet only) |
| Guest login | `[auth] auto_login=yes` (telnet only; PRTERM does not log in as a user) |
| Registered users | `/login` after connect (telnet) |

## Related

| Goal | Doc |
|------|-----|
| PRTERM<->MailboxD wire | [PRTERM-LINK.md](PRTERM-LINK.md) |
| Manual | [MANUAL.md](MANUAL.md) |
