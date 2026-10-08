# Development · MailboxD 1.0.0

Contributor build, test, and code layout reference.

## Workflow matrix

| Step | Command |
|------|---------|
| Debug build | `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build` |
| Tests | `-DMAILBOXD_BUILD_TESTS=ON && ctest --test-dir build` |
| Clients only | `-DMAILBOXD_CLIENTS_ONLY=ON` |
| Format / lint | project scripts in `scripts/` |

## Layout matrix

| Path | Content |
|------|---------|
| `src/core/` | Daemon core — sessions, mail, chat, commands |
| `src/clients/` | CLI clients |
| `plugins/` | Transport plugins (`telnet`, `websocket`) + feature plugins (`entertain`) |
| `include/mailboxd/` | Public headers |
| `share/` | INI templates, YAML registries |
| `tests/` | Unit tests (opt-in build) |

## Dev notes matrix

| Topic | Rule |
|-------|------|
| Wire parsing | Stays in plugins — never in `src/core/` |
| Radio side | none — never add TNC, modem or serial port code (PRTERM's job) |
| Entertain apps | Plugin under `plugins/entertain/` only |
| Live secrets | `./local/` only |

## Related

| Goal | Doc |
|------|-----|
| Build | [BUILD.md](BUILD.md) |
| Contributing | [../CONTRIBUTING.md](../CONTRIBUTING.md) |
