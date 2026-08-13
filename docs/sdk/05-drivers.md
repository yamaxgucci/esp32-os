# Загружаемые драйверы (.SYS)

Как написать модуль, который публикует устройство, и как его загрузить.
Контракт — [`argon/abi.h`](../../sdk/include/argon/abi.h); пример —
[`apps/echo/echo.c`](../../apps/echo/echo.c). Тот же echo можно собрать на
госте через Argon CC: [`apps/cc/examples/echo.c`](../../apps/cc/examples/echo.c)
(`#pragma drv`, `ag_driver_init`, без host GCC).

## Что это

`.SYS` — тот же формат образа, что и `.AXE`, с флагом `AG_AXE_DRIVER`. Точка
входа — `ag_driver_init`, не `ag_main`. Образ остаётся в арене кода, пока его
не выгрузят; устройства, которые он зарегистрировал, уходят вместе с ним.

```c
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("ECHO", "1.0", "you");

static const ag_dev_ops_t k_ops = { .read = ..., .write = ... };

ag_err_t ag_driver_init(void)
{
    const ag_dev_add_t desc = {
        .name = "echo",
        .driver = "ECHO",
        .cls = AG_DEV_CHAR,
        .ops = &k_ops,
        .priv = &my_state,
    };
    return ag_dev_add(&desc);
}
```

`ag_dev_add` разрешён **только** внутри `ag_driver_init`. Снаружи — `-AG_EPERM`:
обычное приложение не драйвер. Владелец устройства — модуль; `drv unload`
отзывает всё, что он опубликовал (`-AG_ENODEV` на следующих вызовах держателей).

## Сборка и загрузка

```
python tools\mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc ^
    --include sdk/include -o build\apps\ECHO.SYS apps\echo\echo.c

argon test -Put "build\sd_card\ECHO.SYS=t:\echo.sys" ^
    "drv load t:\echo.sys" "drv" "dev echo" "drv unload ECHO"
```

Образы клади в `build/apps/` — `mkaxe` сам копирует ещё в `build/sd_card/`
(см. [`docs/user/03-host-share.md`](../user/03-host-share.md)).

В `SYSTEM.CFG` — список на старт (после монтирования носителей):

```
[modules]
device = c:\drv\pcmvirt.sys
device = a:\drv\force.sys
; I2C: bus:addr:path   или   bus:addr:idreg=idval:path
probe  = 0:0x76:a:\drv\bme280.sys
probe  = 0:0x76:0xD0=0x60:a:\drv\bme280.sys
```

`device=` грузит всегда. `probe=` — только если на шине ответил адрес (и, если
указано, совпал байт ID). Перед загрузкой ядро само читает регистр: чужой чип
в арену не попадает. В `ag_driver_init` совпадение доступно через
`ag_probe_hint()` (шина, адрес, id). Пример — `apps/whoami`.

**Установка** (файл на `C:` + строка в `SYSTEM.CFG` + load сейчас):

```
drv install t:\pcmvirt.sys
drv install a:\kbdvirt.sys a:\pcmvirt.sys a:\mousevirt.sys
```

Повторный `drv install` того же драйвера перезаписывает файл и **перезагружает**
модуль в RAM (то же имя в `AG_DRV` → unload + init). После reboot модуль
поднимается сам со стадии `modules`. `drv uninstall <name>` снимает запись и
файл. Временно: `drv load t:\…` (без persist; тоже replace по имени).

Шелл: `drv` (список), `drv load` / `unload` / `install` / `uninstall` / `probe`.
Имя для unload — из заголовка образа (`PCMVIRT`, `ECHO`), не путь к файлу.
`dev` показывает устройства; `run` на `.SYS` отказывает.

Пример audio: [`apps/pcmvirt`](../../apps/pcmvirt) публикует `/dev/pcmvirt`
(TCP WAV → `tools/pcmplay.py`). Ядро по умолчанию даёт `/dev/pcmnull` и
`/dev/pcmmix` (multi-open, sample-wise sum → `pcmvirt`|`pcm0`|`pcmnull`).

## Ограничения

* Код модуля живёт в общей арене (по умолчанию 64 КБ на все приложения и
  модули). Константы — в PSRAM, арену не занимают.
* До восьми модулей сразу (`AG_MODULE_MAX`).
* Нет изоляции: дикий указатель в драйвере портит систему, как и в приложении.
* `probe` пока только I2C. SPI-панели на скан не отвечают — их тип и пины
  остаются в `BOARD.CFG`.
* Класс `AG_DEV_DISPLAY`: soft `fb0` публикует `ag_display_ops_t` (`info` /
  `acquire` / `release` / `flush` / `swap`) через `dev->ops(h)`. Глобальный
  `api->gfx` остаётся удобным синглтоном; class_ops — путь «устройство как
  файл». Остальные классы со своими vtable — ещё впереди.
