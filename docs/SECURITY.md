# Security · MailboxD 1.0.0

Built-in `[security]` — network protection and abuse in one subsystem.

> The `[security] ssh=`, `websocket=` and `circuit=` keys are silently
> ignored. Those transports are gone; if your INI still has the lines,
> they are a no-op.

## Layer matrix

| Layer | What | Ban? |
|-------|------|------|
| **Soft limits** | Output pacing, message size, traffic shaping | **No** |
| **Abuse** | Brute-force, connection flood, excessive spam | **Yes** — short IP/CALLID cool-down |

## Soft limit matrix

| Area | Keys | Effect |
|------|------|--------|
| `[traffic]` | `baud`, `pace_output`, `line_width` | Output pacing |
| `[chat]` | `message_max` | Truncate oversized lines |
| `[mail]` | `max_messages`, `body_max` | Mailbox caps |

## Ban trigger matrix

| Target | Event | Default threshold |
|--------|-------|-------------------|
| **IP** | `login_fail` | 5 / 10 min |
| **IP** | `rate_limit` | 30 / 60 s |
| **CALLID** | `ban_callid=` config | immediate |
| **CALLID** | abuse report (chat/mail flood, register spam, …) | `abuse_maxretry` / `abuse_findtime` |

Optional `iptables` / `nftables` / `hosts` via `ban_backend`.

## Related

| Goal | Doc |
|------|-----|
| Manual `[security]` | [MANUAL.md](MANUAL.md) |
| Topology link auth | [TOPOLOGY.md](TOPOLOGY.md) |
