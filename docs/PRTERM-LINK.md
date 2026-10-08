# PRTERM<->MailboxD link · MailboxD 1.0.0

Wire protocol of the `mailboxd_prterm` plugin (`plugins/mailboxd_prterm/`).
PRTERM is the web front-end; it never speaks telnet to MailboxD. Instead
PRTERM's `src/mailboxdsock.c` connects to a unix-domain socket and drives
MailboxD's `/`-commands directly. INI keys: [MANUAL.md](MANUAL.md)
`[transport.mailboxd_prterm]`.

## Transport

| Item | Value |
|------|-------|
| Socket | `AF_UNIX`, `SOCK_STREAM`, path from `[transport.mailboxd_prterm] bind` (default `/var/mailboxd/prterm.sock`) |
| Permissions | `chmod` to `mode` (default `0660`) after `bind()`; PRTERM's process must share group or supplementary-group access |
| Concurrency | One accepted connection at a time. The accept loop services a client's full request/response cycle before accepting the next; late arrivals queue in the kernel backlog (4) |
| Session identity | Each connection opens one MailboxD session as **Guest** (`mailboxd_session_open`, remote tag `unix:mailboxd-prterm`) — the PRTERM operator is not logged in as a MailboxD user |
| Framing | ASCII lines, `\n`-terminated (a trailing `\r` is tolerated and stripped) |

## Requests

| Request | Reply | Notes |
|---------|-------|-------|
| `HELLO [version]` | `OK MAILBOXD <version>\n` | `<version>` is the plugin's wire version (currently `1`); argument is accepted but not checked |
| `PING` | `PONG\n` | Liveness probe |
| `RUN <command-line>` | zero or more `OUT <line>\n`, then `END ok\n` or `END err <reason>\n` | `<command-line>` must start with `/` (a MailboxD command). The captured `mailboxd_session_write_line()` output of the command becomes the `OUT` lines |
| `RUN` (no argument) | `END err empty-command\n` | |
| `RUN <non-slash text>` | `END err not-a-mailboxd-command\n` | The link only runs `/`-commands, never chat/mail compose lines |
| anything else | `END err unknown-request\n` | |
| line over 2047 bytes | `END err line-too-long\n` | Input buffer is bounded (`MAILBOXD_PRTERM_LINE_MAX`); the oversized line is dropped |

`END err dispatch-failed-<N>` means `mailboxd_command_dispatch()` returned
`mailboxd_result_t` code `<N>` (permission denied, not found, …) — see
`mailboxd_result_name()` in `include/mailboxd/types.h`.

## Example session

```
> HELLO 1
< OK MAILBOXD 1
> PING
< PONG
> RUN /version
< OUT MailboxD 1.0.0
< OUT Operating system: Linux
< END ok
> RUN /who
< OUT Online users:
< OUT   (none)
< OUT Total: 0
< END ok
```

## `link_token`

`[transport.mailboxd_prterm] link_token` is parsed and stored
(`mailboxd_mailboxd_prterm_config_t.link_token`) but **not yet verified**
by the plugin — any local process able to reach the socket path can issue
`RUN` requests. Treat the socket's filesystem permissions (`bind`
directory + `mode`) as the actual access boundary for now. Wiring an
actual `LINK_TOKEN <token>` handshake check into `process_one_line()` is
open follow-up work, not yet implemented.

## Test coverage

`tests/test_mailboxd_prterm.c` (built only with
`-DMAILBOXD_PLUGIN_MAILBOXD_PRTERM=ON`) drives this protocol end-to-end
against a real `mailboxd_service_t`: HELLO/PING/RUN success and every
error reply above, plus a second sequential connection to exercise the
single-client accept loop.
