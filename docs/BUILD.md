# Build · MailboxD 1.0.0

POSIX+ build reference. Platforms: [PLATFORMS.md](PLATFORMS.md).

## Build matrix

| Step | Command |
|------|---------|
| Release build | `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build` |
| Run | `./scripts/mailboxd.sh` |
| Test | `-DMAILBOXD_BUILD_TESTS=ON` → `ctest --test-dir build` |
| Telnet smoke | `telnet 127.0.0.1 2323` |
| Clients only | `-DMAILBOXD_CLIENTS_ONLY=ON` or `./scripts/build-clients.sh` |

## Build outputs

| Output | Role |
|--------|------|
| `mailboxd` | Single local BBX (the historical `mailboxd-secondary` / `mailboxd-proxy` binaries are gone) |
| `mailboxd-telnet` | CLI client |

## CMake options matrix

| Option | Default | Description |
|--------|---------|-------------|
| `MAILBOXD_BUILD_DAEMON` | ON | Daemon + core |
| `MAILBOXD_BUILD_CLIENTS` | ON | CLI clients |
| `MAILBOXD_CLIENTS_ONLY` | OFF | Clients only (no daemon, no plugins) |
| `MAILBOXD_BUILD_PLUGINS` | ON | Transport plugins |
| `MAILBOXD_PLUGIN_TELNET` | ON | TCP/IP telnet transport |
| `MAILBOXD_PLUGIN_MAILBOXD_PRTERM` | OFF | PRTERM<->MailboxD unix-socket link plugin (see [PRTERM-LINK.md](PRTERM-LINK.md)) |
| `MAILBOXD_PLUGIN_ENTERTAIN` | ON | Entertain plugin (chess; forced off on AmigaOS) |
| `MAILBOXD_BUILD_CLIENT_TELNET` | ON | `mailboxd-telnet` client |
| `MAILBOXD_BUILD_TESTS` | OFF | Unit tests |
| `MAILBOXD_HARDENING` | ON | Compiler security hardening |
| `MAILBOXD_WARNINGS_AS_ERRORS` | OFF | Treat warnings as errors |
| `MAILBOXD_CRYPTO_OPENSSL` | OFF | Optional OpenSSL crypto backends |
| `MAILBOXD_CRYPTO_LIBSODIUM` | OFF | Optional libsodium crypto backends |
| `MAILBOXD_STORAGE_SQLITE` | ON | SQLite storage backend |

## Config matrix

| Item | Value |
|------|-------|
| Live INI | `./local/mailboxd.ini` (gitignored) |
| Templates | `share/*.ini.example` |
| Secrets | `./local/` only — never commit |

## Related

| Goal | Doc |
|------|-----|
| Platforms | [PLATFORMS.md](PLATFORMS.md) |
| Developer | [DEVELOPMENT.md](DEVELOPMENT.md) |
