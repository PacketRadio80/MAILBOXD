# tinysha256 (bundled)

Public-domain SHA-256 for MailboxD password hashing.

- `tinysha256.c` / `tinysha256.h` — compiled into `mailboxd_core`
- Adapted from the [983/SHA-256](https://github.com/983/SHA-256) implementation (The Unlicense)

MailboxD stores passwords as `{sha256}` + 64 lowercase hex digits.
