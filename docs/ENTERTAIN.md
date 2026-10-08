# Entertain Area · MailboxD

Optional **Main / standalone Main** feature plugin. Chess is a **fixed** part of
Entertain (no separate `chess=` INI key). AmigaOS builds force the plugin **OFF**.

## Enable

| Layer | Setting |
|-------|---------|
| Build | `-DMAILBOXD_PLUGIN_ENTERTAIN=ON` (default on POSIX; forced off on AmigaOS) |
| INI | **`[entertain] enabled=yes`** (not under `[networks]`) |
| Role | Main or standalone Main only |

```ini
[entertain]
enabled = yes
```

Legacy `[networks] entertain=yes` is still read with a warning — prefer `[entertain]`.

Log: `[entertain] enabled=yes …` then `[entertain] plugin loaded — chess_max_games=…`

When Entertain is off, `/chess` stays **Unknown command** (by design).

## Chess

| Command | Action |
|---------|--------|
| `/chess help` | Command list |
| `/chess new` | Create game (White) |
| `/chess join [id]` | Join as Black |
| `/chess watch [id]` | Spectate |
| `/chess move e2 e4` | Move (`/mv e2 e4` or `/mv e2e4`) |
| `/chess move e7 e8 q` | Promote (q/r/b/n) |
| `/chess resign` / `/chess draw` | Resign / offer-accept draw |
| `/chess list` / `/chess board` / `/chess leave` / `/chess clear` | List / show / stop spectating / free finished slots |
| `/play …` | Alias for `/chess` |
| `/proxychess …` | **Stub** — answers "not implemented yet" |

Limits: no castling, no en passant; pawn promotes to Q/R/B/N. Compact ASCII board.

## Related

| Goal | Doc |
|------|-----|
| Topology | [TOPOLOGY.md](TOPOLOGY.md) |
| Commands | [COMMANDS.md](COMMANDS.md) |
| Areas | `share/areas.yaml` · Entertain menu (User+) |
