# Licensing · MailboxD 1.0.0

MailboxD and shipped components — license reference.

## License matrix

| Component | License |
|-----------|---------|
| MailboxD core + plugins | GPL-3.0-or-later — [LICENSE.txt](../LICENSE.txt), [NON-PROFIT](../NON-PROFIT) |
| Monocypher (`third_party/monocypher/`) | BSD-2-Clause OR CC0-1.0 — header must never be removed |
| tiny-AES-c (`third_party/tinyaes/`) | CC0-1.0 |
| tinysha256 (`third_party/tinysha256/`) | The Unlicense |
| Optional OpenSSL / libsodium / SQLite / pthreads | upstream licenses when linked |

MailboxD is `Non-Profit GNU/GPLv3 and newer Software/Data`. Full third-party
detail: [THIRD-PARTY.md](../THIRD-PARTY.md) · `share/THIRD_PARTY_NOTICES.txt`.

## Distribution matrix

| Item | Rule |
|------|------|
| Source offer | GPL-3.0 compliance required for derivatives |
| Operator config | `./local/` — not part of distribution |
| Third-party deps | Documented in [THIRD-PARTY.md](../THIRD-PARTY.md) |

## Related

| Goal | Doc |
|------|-----|
| Build options | [BUILD.md](BUILD.md) |
| Root README | [../README.md](../README.md) |
