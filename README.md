# MailboxD · 1.0.0
**Official MailboxD Instance - telnet://mailbx1.space:2323**
<br>
**MailboxD** is the mailbox and BBS (BBX) of the PRTERM stack: mail, chat,
conference and the `/` commands in plain-text sessions. It is standalone
additional software to PRTERM — not a part of it.

**MailboxD has no radio side.** It never talks to a TNC, a modem or a serial
port. RF is entirely PRTERM's business; MailboxD only serves users.

> **Status: earlier development (0.5.0).**

---

## Feature matrix

| Feature | Status |
|---------|--------|
| User login (telnet) | yes |
| Mail / chat / conference | yes |
| `/` commands — Sysop → Admin → Mod → User → Guest | yes |
| Entertain area (chess) | yes |
| PRTERM<->MailboxD link (unix-domain socket) | yes — see [PRTERM-LINK.md](docs/PRTERM-LINK.md) |

## Transport matrix

| Transport | Default | Notes |
|-----------|---------|-------|
| `telnet` | port 2323 | CLI users (`mailboxd-telnet`) |
| `mailboxd_prterm` | unix socket `/var/mailboxd/prterm.sock` | PRTERM is the web front-end; not a user transport |

## Build · test · run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
cmake -B build-test -DMAILBOXD_BUILD_TESTS=ON && cmake --build build-test && ctest --test-dir build-test
./scripts/mailboxd.sh
```

## Related

| Goal | Doc |
|------|-----|
| Operate | [MANUAL.md](docs/MANUAL.md) |
| Full index | [docs/README.md](docs/README.md) |

---

Non-Profit GNU/GPLv3 and newer Software/Data
Copyright (C) 2026 MailboxD contributors+

See [LICENSE.txt](LICENSE.txt), [NON-PROFIT](NON-PROFIT) and
[THIRD-PARTY.md](THIRD-PARTY.md).
