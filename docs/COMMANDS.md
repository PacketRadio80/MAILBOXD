# Commands · MailboxD 1.0.0

Slash-command reference — five access levels: Sysop → Admin → Mod → User → Guest.

## Level matrix

| Level | Typical commands |
|-------|------------------|
| Sysop | `/broadcast`, `/shutdown`, `/restart`, `/deleteuser` |
| Admin | `/usercreate`, `/activate`, `/promote`, `/demote`, `/changeuser` |
| Mod | moderation subset |
| User | `/mail`, `/chat`, `/conference`, `/who` |
| Guest | `/help`, `/menu` (filtered) |

## Area matrix

| Group | Commands |
|-------|----------|
| General | `/help` `/menu` `/index` `/alias` `/news` `/banner` `/motd` `/rules` `/who` `/users` `/session` `/version` |
| Screen | `/clear` `/echo` |
| Areas | `/leave` `/main` `/chat` `/conference` `/mail` `/exit` |
| Account | `/login` `/register` `/changeme` `/deleteme` |
| Admin | `/usercreate` `/activate` `/changeuser` `/promote` `/demote` `/delete` |
| Monitor | `/monitor` |
| Entertain | `/chess` (aliases `/play`, `/mv`) |
| Sysop | `/deleteuser` `/shutdown` `/restart` `/broadcast` |

## Broadcast matrix

| Command | Scope | Level |
|---------|-------|-------|
| `/broadcast <msg>` | Local online users | Sysop |

## Registry matrix

| Item | Path |
|------|------|
| Command definitions | `share/commands.yaml` |
| Area definitions | `share/areas.yaml` |
| `/index` | Lists all commands (every account) |
| `/who` | Interactive sessions only |

## Related

| Goal | Doc |
|------|-----|
| Manual | [MANUAL.md](MANUAL.md) |
| Security | [SECURITY.md](SECURITY.md) |
