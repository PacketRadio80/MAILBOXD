# MailboxD · 1.0.0

**MailboxD** is the mailbox and BBS (BBX) of the PRTERM stack: mail, chat,
conference and the `/` commands in plain-text sessions. It is standalone
additional software to PRTERM — not a part of it.

**MailboxD has no radio side.** It never talks to a TNC, a modem or a serial
port. RF is entirely PRTERM's business; MailboxD only serves users.

> **Status: v0.6.5 and mostly developed enough to be used**

## MailboxD-Text / MailboxD-UI

**MailboxD-Text** is the protocol — the text-based command/response layer over
the unix socket (`RUN /command` → `OUT lines` → `END`). It is the engine that
drives everything and remains standalone. Telnet access, the PRTERM bridge, and
all existing features work purely through MailboxD-Text.

**MailboxD-UI** (planned) is a rendering layer on top of MailboxD-Text. PRTERM
acts as the client/slave, consuming the same text stream and rendering it as a
richer UI in the browser. MailboxD's role as the server/master does not change
— it serves the same text protocol, only PRTERM interprets it differently.

Design rules:
- No changes to existing code unless unavoidable; additions only
- MailboxD-Text is the protocol, the engine, and the fallback — always
- MailboxD-UI uses MailboxD-Text as its motor — never replaces it
- Both options coexist; the user chooses text or UI

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
