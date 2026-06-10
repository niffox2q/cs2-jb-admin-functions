### Модуль Admin Functions для Jailbreak Core  
Добавляет административные команды для управления CT-игроками: бан, кик, проверка бана и снятие бана через систему наказаний с MySQL.

Модуль требует **Admin System** для проверки прав доступа. Все permissions уже встроены в модуль и используются напрямую:

- `@admin/jb_banct` — бан игрока в CT
- `@admin/jb_kickct` — кик игрока из CT
- `@admin/jb_checkban` — проверка активного бана
- `@admin/jb_unbanct` — снятие бана

Команды доступны через чат и консоль:
- `!banct`
- `!kickct`
- `!checkban_ct`
- `!unban_ct`

Все данные о банах сохраняются в MySQL (`jb_punishments`) и применяются при входе в CT.

### Установка:
Распаковать в `game/csgo/addons/`

### Требования:
- [Jailbreak Core](https://discord.gg/WkTwuKe8zy)
- [Admin System](https://github.com/Pisex/cs2-admin_system/tree/main)
- [Utils](https://github.com/Pisex/cs2-menus)
