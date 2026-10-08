# Platforms · MailboxD 1.0.0

POSIX+ first — Linux, *BSD, AmigaOS 3.9+, MacOS, Windows.

## Platform matrix

| Platform | Daemon | CLI clients | Notes |
|----------|--------|-------------|-------|
| Linux | yes | yes | Reference platform |
| FreeBSD / NetBSD / OpenBSD | yes | yes | LLVM Clang toolchain default |
| AmigaOS 3.9+ | no | telnet client | `./scripts/build-amiga-telnet.sh` (clients only) |
| MacOS | yes | yes | — |
| Windows | yes | yes | — |

## AmigaOS matrix

| Item | Value |
|------|-------|
| Cross-build | `./scripts/build-amiga-telnet.sh` (`MAILBOXD_CLIENTS_ONLY=ON`) |
| Entertain plugin | forced OFF |
| Full feature set | Linux / *BSD recommended |

## Related

| Goal | Doc |
|------|-----|
| Build | [BUILD.md](BUILD.md) |
| Manual | [MANUAL.md](MANUAL.md) |
