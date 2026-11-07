# Сборка ArgonOS: плата за платой

[English](building.md) | **Русский**

Эта страница ведёт от свежего клона репозитория до платы с приглашением `C:\>`.
Первая часть общая для всех плат, потом у каждой платы свой короткий список шагов.

## Что нужно

| | |
|---|---|
| Компьютер | **Windows 10/11.** Раннер задач — PowerShell; других хостов для сборки пока нет. |
| ESP-IDF | **5.5 или новее**, установленный установщиком Espressif. Скрипт `argon` ищет его в обычных местах, либо путь указывается явно (ниже). |
| Python | 3.10 или новее, с `pip`. |
| Компилятор C на ПК | Только для `argon tests` (юнит-тесты на ПК). Подойдёт MinGW-w64. |
| Кабель USB | Именно с данными: многие кабели только питают. |

Пути, специфичные для машины, лежат в `tools\local-env.ps1` (он не в репозитории):

```powershell
$env:ARGON_IDF_PATH       = 'D:\Espressif\esp-idf'
$env:ARGON_IDF_TOOLS_PATH = 'D:\Espressif\tools'
$env:ARGON_PYTHON         = 'D:\dev\python312\python.exe'
$env:ARGON_HOST_CC_BIN    = 'D:\dev\mingw64\bin'
```

Если файла нет, `tools\idf-env.ps1` угадывает пути сам.

```powershell
git clone https://github.com/yamaxgucci/esp32-os.git
cd esp32-os
.\argon.cmd help
```

## Общий рецепт

Любая плата проходит одни и те же пять шагов. Меняются только *цель* и
*пакет платы*.

```powershell
# 1. Выбрать плату. Стирает build\ и перенастраивает проект под чип.
.\argon.cmd target <цель>

# 2. Собрать прошивку.
.\argon.cmd build

# 3. Прошить. COM-порт — ваш.
.\argon.cmd flash -port COM5 -NoMonitor

# 4. Собрать программы и драйверы, которые лягут во flash платы (диск C:).
.\argon.cmd apps

# 5. Записать C: — там карта выводов платы, конфигурация загрузки и драйверы,
#    которые система грузит при старте.
python tools\mksysfs.py --board boards\<пакет-платы> --apps build\apps `
    --sdkconfig sdkconfig --flash -p COM5

# Посмотреть.
.\argon.cmd monitor -port COM5
```

* Консоль — **115200 8N1**. Подойдёт монитор ESP-IDF (выход `Ctrl+]`), PuTTY или
  любой терминал с ANSI-последовательностями: экран рисуется ими.
* **`argon target` удаляет `build\` и `sdkconfig`.** Смена платы — полная
  пересборка. `argon target` без аргумента покажет, на что настроено сейчас.
* `C:` форматируется заново при смене прошивки или таблицы разделов, поэтому
  шаг 5 каждый раз идёт после шага 3.
* Шаг 5 сообщает системе, на каких выводах экран и SD-карта. Плата без него
  загрузится до приглашения, но экран останется тёмным.

Что должно появиться:

```
ArgonOS 0.1.0 (…)  esp32s3  profile=full
… KB conventional memory, 8192 KB extended
ABI 0.xx  cores=2  app core=1
…
Type 'help' for a list of commands.
C:\>
```

Вторая строка — первая проверка железа. *Extended* — это PSRAM: если там ноль на
плате, где она есть, неверен режим (octal против quad). Если что-то не
поднялось — [user/02-board-setup.md](user/02-board-setup.md), раздел 8.

## Без платы: QEMU

```powershell
.\argon.cmd target esp32s3
.\argon.cmd build
.\argon.cmd run                       # консоль в этом окне; Ctrl+A, затем X — выход
.\argon.cmd test ver mem              # скриптовая загрузка, печатает получившийся экран
.\argon.cmd tests                     # юнит-тесты на ПК
.\argon.cmd check                     # всё вместе плюс все приложения
```

QEMU запускает цель ESP32-S3 (и вариант для ESP32-C3, `esp32c3-qemu`). C6 и P4 он
не запускает. Всё, что зависит от настоящих часов, выводов или панели, требует
платы.

## Платы

| Плата | Цель | Пакет платы | Нужно |
|---|---|---|---|
| [ESP32-S3-WROOM CAM (N16R8)](#esp32-s3-cam) | `esp32s3-board` | `esp32s3-cam` | 8 МБ octal PSRAM |
| [ESP32 «CYD» 2432S028R / S024](#cyd) | `esp32-cyd` | `esp32-cyd` | — |
| [Waveshare ESP32-S3-Touch-LCD-2](#touch-lcd-2) | `esp32s3-lcd2` | `esp32s3-touch-lcd-2` | — |
| [Waveshare ESP32-S3-Zero](#s3-zero) | `esp32s3-zero` | `esp32s3-zero` | I2S ЦАП для звука |
| [Waveshare ESP32-C6-LCD-1.47](#c6) | `esp32c6` | `esp32-c6-lcd-1.47` | — |
| [ESP32-C3 SuperMini](#c3) | `esp32c3` | `esp32c3-supermini` | — |
| [M5Stack Stamp-P4](#p4) | `esp32p4` | `m5stack-stamp-p4` | — |
| [ESP32-A1S Audio Kit](#a1s) | `esp32-a1s` | `esp32-a1s` | — |
| [USB-KVM-свисток (S3-CAM или T-Dongle-S3)](#kvm) | `esp32s3-kvm` | `esp32s3-kvm` / `esp32s3-dongle` | — |

Карты выводов в пакетах плат — это платы, что были на столе автора. Клоны одного
семейства различаются: у 2.8" CYD подсветка на одном выводе, у 2.4" на другом, —
поэтому **сверьте `BOARD.CFG` пакета со схемой вашей платы, прежде чем ему
верить**. Неверный chip select SD — это карта, которая не монтируется, пока всё
остальное выглядит здоровым.

<a id="esp32-s3-cam"></a>
### ESP32-S3-WROOM CAM (N16R8)

Основная плата разработки: 16 МБ flash, 8 МБ octal PSRAM, разъём камеры, слот
microSD. Экран — отдельный модуль ST7789 на гребёнке камеры. Здесь идут Fallout 2
и Mega Drive. Подробнее: [14-esp32-s3-cam.md](14-esp32-s3-cam.md).

```powershell
.\argon.cmd target esp32s3-board
.\argon.cmd build
.\argon.cmd flash -port COM4 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32s3-cam --apps build\apps --sdkconfig sdkconfig --flash -p COM4
```

* `esp32s3-board` — S3 без двух настроек, существующих только для QEMU.
  `esp32s3-cam` — то же, но с камерой в образе; выводы камеры и SPI-панели на
  гребёнке общие, поэтому одновременно нельзя.
* Модуль с **32 МБ** PSRAM не работает (см. README). Для **quad** PSRAM нужен
  `CONFIG_SPIRAM_MODE_QUAD` вместо `…_OCT`, иначе PSRAM не найдётся.

<a id="cyd"></a>
### ESP32 «CYD» (ESP32-2432S028R / S024)

ESP32-WROOM-32, 4 МБ flash, **PSRAM нет**, экран ILI9341 320×240 по SPI,
резистивный тач, слот SD по SPI. Самый дешёвый способ увидеть систему в работе.
Подробнее: [09-esp32-cyd.md](09-esp32-cyd.md).

Проще всего: скачайте образ со страницы
[Releases](https://github.com/yamaxgucci/esp32-os/releases) и пройдите
[Прошивка готового образа](#прошивка-готового-образа) ниже.

Сборка самому:

```powershell
.\argon.cmd target esp32-cyd
.\argon.cmd build
.\argon.cmd flash -port COM3 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32-cyd --apps build\apps --sdkconfig sdkconfig --flash -p COM3
```

* Варианты цели: `esp32-cyd` — **без радио**, памяти больше всего; `esp32-cyd-net`
  добавляет Wi-Fi и клиент магазина; `esp32-cyd-md` — это `-net` с дополнительной
  instruction RAM для Mega Drive. Mega Drive идёт на обычной `esp32-cyd`.
* `BOARD.CFG` пакета — для модели 2.4". Если у вас 2.8" (S028), сначала проверьте
  выводы подсветки и SD.

<a id="touch-lcd-2"></a>
### Waveshare ESP32-S3-Touch-LCD-2

ESP32-S3R8: 16 МБ flash, 8 МБ octal PSRAM, панель ST7789 2″ 240×320, ёмкостный
тач, карта TF. Экран и карта сидят на одной шине SPI.

```powershell
.\argon.cmd target esp32s3-lcd2
.\argon.cmd build
.\argon.cmd flash -port COM6 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32s3-touch-lcd-2 --apps build\apps --sdkconfig sdkconfig --flash -p COM6
```

Сначала прочитайте заметки внизу `boards\esp32s3-touch-lcd-2\BOARD.CFG`: там
описано, как разведён единственный разъём USB-C. См. также
[user/02-board-setup.md](user/02-board-setup.md).

<a id="s3-zero"></a>
### Waveshare ESP32-S3-Zero

4 МБ flash, без экрана и без слота карты, консоль по USB-JTAG. С I2S ЦАП CS4344 на
выводах из `BOARD.CFG` звучит. У неё своя таблица разделов
(`partitions_4mb_zero.csv`). Обычно — узел [кластера](user/07-cluster.md).

```powershell
.\argon.cmd target esp32s3-zero
.\argon.cmd build
.\argon.cmd flash -port COM6 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32s3-zero --apps build\apps --sdkconfig sdkconfig --flash -p COM6
```

<a id="c6"></a>
### Waveshare ESP32-C6-LCD-1.47

RISC-V, одно ядро на 160 МГц, 4 МБ flash, PSRAM нет, экран ST7789 172×320 и слот
microSD. Разъём USB-C идёт прямо в чип, поэтому консоль — собственный USB-serial
чипа. Wi-Fi 6 и BLE работают оба. Подробнее:
[12-esp32-c6-lcd.md](12-esp32-c6-lcd.md).

```powershell
.\argon.cmd target esp32c6
.\argon.cmd build
.\argon.cmd flash -port COM5 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32-c6-lcd-1.47 --apps build\apps `
    --sdkconfig sdkconfig --add boards\esp32-c6-lcd-1.47\AUTOEXEC.BAT --flash -p COM5
```

* **QEMU этот чип не запускает.** Перед коммитом и перед тестами вернитесь
  командой `argon target esp32s3`.
* Чтобы прошить пустую плату, возможно, придётся удерживать **BOOT** (GPIO 9) при
  подключении, чтобы чип вошёл в загрузчик ROM.

<a id="c3"></a>
### ESP32-C3 SuperMini

RISC-V, одно ядро, около 400 КБ RAM, тринадцать выводов, без экрана, карты и
звука. Хороший второй узел и плохой первый: держит программу и провод, а экран
одалживает у соседа. Консоль — по USB чипа.

```powershell
.\argon.cmd target esp32c3
.\argon.cmd build
.\argon.cmd flash -port COM8 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32c3-supermini --apps build\apps --sdkconfig sdkconfig --flash -p COM8
```

Про стол из двух плат — [user/07-cluster.md](user/07-cluster.md).

<a id="p4"></a>
### M5Stack Stamp-P4

ESP32-P4 (rev 1.3): два ядра RISC-V на 360 МГц, 32 МБ PSRAM, 16 МБ flash,
**радио нет**, на модуле больше ничего: ни экрана, ни карты, ни кнопок. Разъём
USB-C — USB-Serial/JTAG самого чипа. Подробнее:
[17-esp32-p4-stamp.md](17-esp32-p4-stamp.md).

```powershell
.\argon.cmd target esp32p4
.\argon.cmd build
.\argon.cmd flash -port COM21 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\m5stack-stamp-p4 --apps build\apps --sdkconfig sdkconfig --flash -p COM21
```

QEMU этот чип не запускает. Перед тестами вернитесь на `esp32s3`.

<a id="a1s"></a>
### ESP32-A1S Audio Kit

Аудиоплата AI-Thinker (маркировка V2.2) с кодеком ES8388, гитарным и
наушниковым разъёмами, без экрана. Плата, в которой должен жить эмулятор лампового
усилителя. Кабель — в micro-USB с надписью **UART**; тот, что с надписью `power`,
данных не несёт. У Windows нет драйвера для CP2102 на ней — поставьте VCP-драйвер
Silicon Labs. Автосброс работает, кнопку BOOT держать не нужно. Подробнее:
[15-esp32-a1s.md](15-esp32-a1s.md).

```powershell
.\argon.cmd target esp32-a1s          # педаль: без радио, эмулятор на полной скорости
# .\argon.cmd target esp32-a1s-net    # с Wi-Fi, чтобы телефон стал экраном
.\argon.cmd build
.\argon.cmd flash -port COM12 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32-a1s --apps build\apps --sdkconfig sdkconfig --flash -p COM12
```

Под одной маркировкой «V2.2» продавались платы как минимум с двумя разными
кодеками и картами выводов. `BOARD.CFG` пакета намеренно почти пустой; прежде чем
его заполнять, прочитайте документ выше.

<a id="kvm"></a>
### USB-KVM-свисток

Плата втыкается в USB другого компьютера и представляется клавиатурой, мышью и
флешкой; экран того компьютера показывается на телефоне, а ввод с телефона
уходит обратно. На компьютере не запускается никакого ПО, он видит клавиатуру.
Работает в BIOS. Прошивка идёт на S3-CAM и на LilyGo T-Dongle-S3 (отличается
только маленький экран). Подробнее: [14-usb-kvm.md](14-usb-kvm.md) и
[apps/kvm/README.md](../apps/kvm/README.md).

```powershell
.\argon.cmd target esp32s3-kvm
.\argon.cmd build
.\argon.cmd flash -port COM3 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32s3-dongle --apps build\apps --sdkconfig sdkconfig --flash -p COM3
```

Для S3-CAM вместо `esp32s3-dongle` — `boards\esp32s3-kvm`. У KVM-сборки своя
таблица разделов (`partitions_kvm.csv`).

## Прошивка готового образа

Готовый образ — вся flash одним файлом: загрузчик, таблица разделов, прошивка и
диск `C:`, пишется по смещению 0. ESP-IDF не нужен, нужен только Python.

```powershell
pip install esptool
python -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 argonos-esp32-cyd.bin
```

Порт подставьте свой; на Linux и macOS это `/dev/ttyUSB0` или похожий. В каждом
релизе указана SHA-256 файла. `write_flash 0x0` заменяет **всё** во flash платы,
включая то, что было на `C:`. После сброса консоль — 115200 8N1 на USB-serial
порту.

## Если не работает

| Симптом | Куда смотреть |
|---|---|
| `argon` пишет, что Python не найден | `ARGON_PYTHON` в `tools\local-env.ps1`; заглушка Microsoft Store — это не Python |
| `idf.py` не найден | `ARGON_IDF_PATH` и что установщик ESP-IDF дошёл до конца |
| Прошивка виснет на «Connecting…» | держите BOOT на старте; другой кабель, другой порт |
| Приглашение есть, экран тёмный | пропущен шаг 5; запишите `C:` через `mksysfs` |
| `extended` равно 0 на плате с PSRAM | режим PSRAM: octal против quad |
| `media FAILED` с картой в слоте | выводы SD в `BOARD.CFG`; попробуйте `width = 1` и меньший `max_khz` |
| Мусор в консоли | скорость 115200 и терминал с ANSI |
| `argon target` падает с «отказано в доступе» | что-то под `build\` только для чтения; скрипт сам удаляет `build\` и повторяет |

Подробнее: [user/02-board-setup.md](user/02-board-setup.md).
