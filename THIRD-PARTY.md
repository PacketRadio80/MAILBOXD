# Third-party components

MailboxD itself is the author's own work and is licensed under the GNU General
Public License v3.0 or later, non-profit — see [`LICENSE.txt`](LICENSE.txt) and
[`NON-PROFIT`](NON-PROFIT).

The components below are **not** part of MailboxD's authorship. They keep their
own licenses. Those licenses are compatible with GPL-3.0-or-later, but they are
not overridden by it.

---

## Vendored source

Code that lives inside this tree under `third_party/`.

### Monocypher

| | |
|---|---|
| **Path** | `third_party/monocypher/` |
| **Upstream** | <https://monocypher.org> |
| **Author** | Loup Vaillant |
| **Copyright** | Copyright (c) 2017-2019 Loup Vaillant |
| **License** | BSD-2-Clause **OR** CC0-1.0 (dual-licensed, at your option) |

Used for the `crypted` session transport. The complete license text is in the
header of `monocypher.c` and `monocypher.h`, including the
`SPDX-License-Identifier: BSD-2-Clause OR CC0-1.0` marker. **These two files must
never be relicensed.**

### tiny-AES-c

| | |
|---|---|
| **Path** | `third_party/tinyaes/aes.c`, `third_party/tinyaes/aes.h` |
| **Upstream** | <https://github.com/kokke/tiny-AES-C> |
| **License** | CC0-1.0 (public domain dedication) |

Provides the AES block cipher.

### tinysha256

| | |
|---|---|
| **Path** | `third_party/tinysha256/` |
| **Upstream** | <https://github.com/983/SHA-256> |
| **License** | The Unlicense (public domain) |

Adapted for password hashing. MailboxD stores passwords as `{sha256}` followed
by 64 lowercase hex digits.

---

## Linked libraries

Linked at build or runtime. None of their source is in this tree.

| Library | License | Used for | Required |
|---|---|---|---|
| **SQLite** | Public Domain (blessing) | message and mail storage | optional (`MAILBOXD_STORAGE_SQLITE`) |
| **OpenSSL** | Apache-2.0 | `wss` in the websocket transport, self-signed certificates | optional |
| **libsodium** | ISC | alternative crypto backend | optional |
| **pthreads** | system | threads | yes |

Each is detected at configure time through `pkg-config` / `find_package` and is
skipped when absent — the daemon still builds and runs without them.

---

## Project-written glue

These live in `third_party/` but are MailboxD's own code and carry the project
license (`GPL-3.0-or-later`):

- `third_party/tinyaes/aes256gcm.c` / `.h` — AES-256-GCM wrapper per NIST SP 800-38D
- `third_party/tinyaes/mailboxd_aes_config.h` — tiny-AES-c configuration header
  (AES-256, ECB + CTR enabled, CBC off)

---

## Notes

- **Modifications are limited to adaptation**, and they are listed here:
  - `third_party/tinyaes/aes.h` — one line changed: it includes
    `mailboxd_aes_config.h` so the cipher is configured for AES-256 with ECB + CTR.
  - product-name mentions in `third_party/tinysha256/` refer to this project.
  Apart from that, vendored files are as upstream shipped them.
- **Monocypher's copyright and license header must never be removed.** It is
  dual-licensed BSD-2-Clause OR CC0-1.0; the BSD option requires the notice to
  stay. tiny-AES-c (CC0-1.0) and tinysha256 (The Unlicense) are public domain and
  may be adapted freely — which is why only Monocypher carries that hard rule.
- When a component is ever replaced or removed, update this file in the same
  commit.
