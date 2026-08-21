# Справочник по API

Полный разбор [`argon/abi.h`](../../sdk/include/argon/abi.h) по подсистемам: что
делает каждый вызов, что возвращает и чего **не** делает. Единственный источник
истины — сам заголовок; здесь то, что по нему не видно.

Текущая версия ABI — **0.7**. Правила совместимости и то, как приложение
проверяет наличие вызова, — в разделе [Версии и проверка возможностей](#версии-и-проверка-возможностей).

## Как устроен доступ к системе

У приложения нет ни libc, ни таблицы импортов, ни поиска по именам. Есть **одно
слово** в его образе — `g_ag_api`, — куда загрузчик перед вызовом `ag_main`
записывает адрес таблицы:

```c
const ag_api_t *api = ag_api();       /* то же, что g_ag_api */
api->con->printf("%d\n", 42);         /* прямо через таблицу */
ag_printf("%d\n", 42);                /* то же самое, обёрткой */
```

Таблица — дерево из двух уровней: корень `ag_api_t` и подтаблицы по подсистемам.
Каждый вызов — один косвенный переход; обёртки из
[`argon/argon.h`](../../sdk/include/argon/argon.h) объявлены `static inline`, так
что ничего лишнего в вызов не добавляют. Пользоваться лучше обёртками: имена
короче и не приходится помнить, где что лежит.

Обёртки есть не у всех вызовов — у редко нужных нет (`sys->vlog`, `sys->sym`,
`mem->usable_size`, `fs->truncate`, `con->write`, `con->vprintf`,
`time->get_datetime`, `task->sleep_ms`, `task->self`,
`task->critical_*`, `proc->enumerate`, `proc->foreground`). Они вызываются через
таблицу напрямую, и это нормальный способ, а не обход:

```c
ag_api()->task->sleep_ms(10);
```

**Обёртка не проверяет, что подсистема есть.** `ag_gfx_acquire()` при
`gfx == NULL` — это разыменование нуля, а не отказ с кодом. У подсистем,
помеченных ниже как отсутствующие, наличие проверяется до вызова:

```c
if (ag_api()->gfx != NULL) { ... }        /* есть ли подсистема */
if (AG_HAS(ag_api()->inp, key_pressed)) { ... }   /* есть ли вызов в ней */
```

**Никакого перехода в другой режим при вызове не происходит.** Это не системный
вызов через прерывание, а обычный вызов функции ядра из кода приложения, на
одном и том же уровне привилегий. Отсюда и цена — она нулевая, — и следствие:
неверный указатель, переданный в систему, повреждает систему. Защиты нет и не
планируется, это сознательный выбор модели DOS.

## Подтаблицы: что есть и чего нет

Подсистема, которой ещё нет, — это `NULL`, а не заглушка, возвращающая ошибку.
Приложение может спросить и приспособиться.

| Подтаблица | Состояние | Что в ней |
|---|---|---|
| [`sys`](#sys--система) | ✅ | идентификация, журнал, завершение, `strerror`, heartbeat |
| [`mem`](#mem--память) | ✅ | арена процесса |
| [`fs`](#fs--файлы) | ✅ | файлы, каталоги, монтирование |
| [`con`](#con--консоль) | ✅ | текстовый экран, ввод символами, кодовая страница |
| [`inp`](#inp--ввод-событиями) | ⚠ | события ввода; `key_pressed` — `NULL` |
| [`time`](#time--время) | ⚠ | часы и задержки; `set_datetime`, таймеры — `NULL` |
| [`task`](#task--потоки) | ✅ | потоки, мьютексы, семафоры, очереди |
| [`proc`](#proc--процессы) | ⚠ | процессы; `getenv`/`setenv` — `NULL` |
| [`dev`](#dev--устройства) | ✅ | реестр, `/dev`, `ioctl`; `add`/`remove` для `.SYS` |
| [`io`](#io--железо-напрямую) | ⚠ | GPIO, прерывания, I2C, SPI, UART, PWM; `adc_read` — `NULL` без `CONFIG_ARGON_ENABLE_ADC` |
| [`gfx`](#gfx--графика) | ✅ | soft RGB565 framebuffer; панели SPI — позже |
| `cfg` | ⬜ `NULL` | доступ к `SYSTEM.CFG` из приложения |
| [`power`](#power--частота-экран-и-оповещение) | ✅ 0.34 | что сейчас установлено, о чём система собирается попросить, и ответ на это |
| `net` | ✅ 0.12, `resolve` 0.33 | TCP listen/accept/connect/send/recv, `set_nonblock`, `resolve` (имя или дотированный квартет в адрес). OpenEth в QEMU, Wi-Fi на плате; `ARGON_ENABLE_NET`. Что этим уже сделано в самой системе — [`10-network.md`](../10-network.md) |

## Версии и проверка возможностей

```c
#define AG_ABI_MAJOR 0u
#define AG_ABI_MINOR 10u
```

* **minor + 1** — таблица выросла: новый вызов в конце подтаблицы, новая
  подтаблица в конце корня, или подтаблица перестала быть `NULL`;
* **major + 1** — всё остальное: изменение сигнатуры, удаление, перестановка.

Загрузчик пускает образ, если `major` совпал, а `minor` образа **не больше**
системного. Собранное против 0.1 работает на 0.2; собранное против 0.8 на 0.7 —
нет, и получает `built for a different version of this system`.

Проверять наличие конкретного вызова нужно не по версии, а по таблице — каждая
подтаблица начинается с `size`:

```c
if (AG_HAS(ag_api()->inp, key_pressed)) {
    /* есть и не NULL */
} else {
    /* обойтись без него */
}
```

`AG_HAS` проверяет три вещи разом: подтаблица не `NULL`, её `size` достаточен,
чтобы поле существовало, и само поле не `NULL`. Версии в этой проверке нет
намеренно: «есть ли вызов» — вопрос о таблице, а не о номере.

## Коды ошибок

Всё, что возвращает `ag_err_t`, возвращает `0` (`AG_OK`) при успехе и
**отрицательное** `-AG_Exxx` при отказе. То же и для функций, возвращающих
размер или дескриптор: `>= 0` — результат, `< 0` — ошибка.

```c
const ag_handle_t h = ag_open(path, AG_O_RDONLY);
if (h < 0) {
    ag_printf("%s: %s\n", path, ag_strerror(h));   /* «not found» и т. п. */
    return 1;
}
```

| Код | Значение | Когда встречается чаще всего |
|---|---|---|
| `AG_EPERM` 1 | не разрешено | ввод из фонового процесса |
| `AG_ENOENT` 2 | нет такого файла, устройства или записи | открытие, `stat`, конец перечисления |
| `AG_EIO` 5 | ошибка носителя | нет карты, сбой чтения |
| `AG_EBADF` 9 | неверный дескриптор | работа после `close` |
| `AG_EAGAIN` 11 | сейчас нельзя, попробуйте позже | |
| `AG_ENOMEM` 12 | нет памяти | `alloc`, загрузка образа |
| `AG_EACCES` 13 | доступ запрещён | запись в файл, открытый только на чтение |
| `AG_EBUSY` 16 | занято | каталог не пуст; процесс держит мьютекс |
| `AG_EEXIST` 17 | уже существует | `mkdir`, `AG_O_EXCL` |
| `AG_ENODEV` 19 | нет такого устройства | |
| `AG_ENOTDIR` 20 | не каталог | `chdir` на файл |
| `AG_EISDIR` 21 | это каталог | чтение каталога как файла |
| `AG_EINVAL` 22 | неверный аргумент | `NULL`, битый путь |
| `AG_ENFILE` 23 | таблица дескрипторов полна | |
| `AG_ENOSPC` 28 | нет места | запись на полный диск |
| `AG_EROFS` 30 | только чтение | |
| `AG_ERANGE` 34 | не влезает | буфер меньше пути |
| `AG_ENOSYS` 38 | не реализовано в этой сборке | |
| `AG_ENOTSUP` 45 | не поддерживается устройством | образ для другой архитектуры |
| `AG_ETIMEDOUT` 60 | вышло время | `wait`, `join`, `mutex_lock` |
| `AG_EABI` 90 | несовпадение ABI | |
| `AG_EFORMAT` 91 | битый формат файла или образа | |
| `AG_EKILLED` 92 | процесс снят супервизором | результат `wait` |

`sys->strerror` переводит код в короткую английскую фразу. Текст — для человека,
решения принимаются по коду.

---

## `sys` — система

```c
void        info(ag_sysinfo_t *out);
void        log(ag_log_level_t lvl, const char *tag, const char *fmt, ...);
void        vlog(ag_log_level_t lvl, const char *tag, const char *fmt, va_list ap);
void        exit(int code);                  /* не возвращается */
void        panic(const char *msg);          /* не возвращается */
void       *sym(const char *name);
const char *strerror(ag_err_t err);
void        heartbeat(void);
```

**`info`** заполняет `ag_sysinfo_t`: имя и версия системы, строка сборки
(`git describe`), чип, плата, профиль, частота, число ядер, **номер ядра
приложения** и версия ABI. Всё — фиксированные массивы `char`, никаких
указателей, которые могли бы устареть.

**`log`** пишет в системный журнал, а не на экран. Разница существенная: экран
теряет всплески (рендерер отправляет текущее состояние строки), журнал — нет.
`tag` короткий, обычно имя приложения; `NULL` превращается в `"app"`. Уровни:
`AG_LOG_ERROR`, `WARN`, `INFO`, `DEBUG`, `TRACE`. Обёртка — `ag_log(...)`,
макрос, а не функция, потому что берёт `...`.

**`exit`** завершает процесс с кодом, **не возвращается**. Возврат из `ag_main`
делает то же самое, и обычно так и надо. Ресурсы — память, файлы, потоки, замки
— возвращаются системой в любом случае, забыть об этом нельзя даже нарочно.

**`panic`** — «дальше некорректно продолжать»: пишет причину в журнал уровнем
`ERROR` и завершает процесс. Дампа памяти нет; запись о падении с адресом отказа
делает система сама, когда падение настоящее (см.
[03-application-anatomy.md](03-application-anatomy.md#падение)).

**`sym`** сейчас всегда возвращает `NULL`. Задел на необязательные точки входа;
проверять результат обязательно.

**`heartbeat`** — «я жив» для watchdog'а, который приложение само себе завело
через `proc->watchdog`. Без заявки не значит ничего.

## `mem` — память

```c
void  *alloc(size_t bytes);
void  *alloc_caps(size_t bytes, uint32_t caps);
void  *realloc(void *ptr, size_t bytes);
void   free(void *ptr);
size_t usable_size(const void *ptr);
void   info(ag_meminfo_t *out);
```

Память приходит **из арены процесса**, а не из общей кучи. Это даёт две вещи,
которых иначе не получить: при завершении процесса всё возвращается одной
операцией, чем бы процесс ни закончился, и приложение не фрагментирует кучу
ядра.

Размер арены — 1 МБ по умолчанию, или тот, который образ запросил в заголовке
(`heap_size`). Разница между «по умолчанию» и «запросил» в том, что происходит,
когда столько памяти нет: запрошенный размер обязателен, и приложение получает
отказ при запуске, а размер по умолчанию система уменьшает вдвое, пока не влезет,
но не ниже 16 КБ. Приложение, которому нужен объём, обязано его запросить —
получить меньше и узнать об этом на середине работы хуже, чем не запуститься.

**Не путать с ареной кода.** `arena_*` в `ag_meminfo_t` — это куча *этого
процесса*, из которой отвечает `alloc`. Арена кода — другое: это исполняемая
память, куда загрузчик кладёт код, её 64 КБ на все процессы, и она видна не
здесь, а в команде `mem` шелла.

`caps` в `alloc_caps` — набор из `ag_mem_caps`: `AG_MEM_FAST` (internal SRAM,
низкая задержка), `AG_MEM_DMA`, `AG_MEM_EXEC`, `AG_MEM_ZERO`, `AG_MEM_ALIGN32`.
Запрос, который нельзя выполнить, возвращает `NULL` — не «что-нибудь похожее».

`info` заполняет `ag_meminfo_t`: `arena_total`, `arena_free`, `arena_largest`
(крупнейший непрерывный блок — по нему видно фрагментацию), `fast_free`
(доступная internal SRAM) и `system_free` (свободное у ядра, справочно).

Чего здесь нет: `AG_MEM_EXEC` пока никем не отдаётся — исполняемая память это
арена кода, и она принадлежит загрузчику. `usable_size` возвращает то, что
реально выделено, — оно бывает больше запрошенного.

## `fs` — файлы

```c
ag_handle_t open(const char *path, uint32_t flags);
ag_err_t    close(ag_handle_t h);
int32_t     read(ag_handle_t h, void *buf, size_t len);
int32_t     write(ag_handle_t h, const void *buf, size_t len);
int64_t     seek(ag_handle_t h, int64_t off, int whence);
ag_err_t    sync(ag_handle_t h);
ag_err_t    truncate(ag_handle_t h, uint64_t len);

ag_err_t    stat(const char *path, ag_stat_t *out);
ag_err_t    unlink(const char *path);
ag_err_t    rename(const char *from, const char *to);
ag_err_t    mkdir(const char *path);
ag_err_t    rmdir(const char *path);

ag_handle_t opendir(const char *path);
ag_err_t    readdir(ag_handle_t h, ag_dirent_t *out);
ag_err_t    closedir(ag_handle_t h);

ag_err_t    getcwd(char *buf, size_t len);
ag_err_t    chdir(const char *path);
ag_err_t    mountinfo(const char *mount, ag_fsinfo_t *out);
```

**Пути.** Внутри система работает с POSIX-путями (`/tmp/x.txt`), а буквы диска —
представление шелла: `T:\x.txt` и `/tmp/x.txt` — одно и то же, принимается и то,
и другое, разделитель может быть любым. Относительный путь разрешается от
рабочего каталога **вызывающего процесса**, а не шелла. Предел — `AG_PATH_MAX`
(256) на путь и `AG_NAME_MAX` (64) на имя; они в ABI именно потому, что
приложение, вызывающее `getcwd`, обязано знать границу.

Диски: `A:` — карта памяти (`/sd`), `C:` — внутренний flash (`/sys`), `T:` —
диск в памяти (`/tmp`), `D:` — устройства (`/dev`, см.
[`dev`](#dev--устройства)).

**`open`** — флаги из `ag_open_flags`: `AG_O_RDONLY` (0), `AG_O_WRONLY`,
`AG_O_RDWR`, `AG_O_CREATE`, `AG_O_TRUNC`, `AG_O_APPEND`, `AG_O_EXCL`. Возвращает
дескриптор `>= 0` или отрицательный код.

**`read`/`write`** возвращают число байт, `0` — конец файла, отрицательное —
ошибка. **Короткое чтение законно** и в середине файла: цикл, который считает,
что один `read` вернёт всё запрошенное, работает по случайности.

**`seek`** возвращает новое положение (`int64_t`) или отрицательный код.
`whence`: `AG_SEEK_SET`, `AG_SEEK_CUR`, `AG_SEEK_END`.

**`sync`** сбрасывает записанное на носитель. `close` этого не обещает: если
важно, что байты дошли, — `sync` до `close`.

**`readdir`** отдаёт `ag_dirent_t` (имя и `ag_stat_t`) и возвращает `-AG_ENOENT`,
когда записи кончились: это не ошибка, а конец перечисления. Порядок записей —
тот, в котором их отдаёт файловая система, не отсортированный. `.` и `..` не
подставляются.

**`stat`** заполняет размер, `mtime` (unix-секунды, `0` если неизвестно) и
`attr` — набор из `AG_A_DIR`, `AG_A_READONLY`, `AG_A_HIDDEN`, `AG_A_SYSTEM`.

**`mountinfo`** по точке монтирования (`"/tmp"`) или `NULL` — про первый диск —
заполняет `ag_fsinfo_t`: тип (`"fat"`, `"ram"`, `"lfs"`, `"dev"`), общий и
свободный объём, признаки «только чтение» и «съёмный».

Чего здесь нет: длинных имён сверх FAT-ограничений там, где файловая система FAT
(в этой сборке ESP-IDF LFN включены, но это свойство сборки, а не ABI);
блокировок файлов; прав доступа; символических ссылок; рекурсивных операций —
удаление дерева пишется в приложении, и `fm` именно так и делает.

## `con` — консоль

```c
int32_t write(const char *buf, size_t len);
int32_t puts(const char *s);
int32_t printf(const char *fmt, ...);
int32_t vprintf(const char *fmt, va_list ap);

int32_t getch(void);
int32_t kbhit(void);
int32_t readline(char *buf, size_t len);

void cls(void);
void gotoxy(uint16_t x, uint16_t y);
void set_attr(uint8_t attr);
void set_cursor(bool visible);
void info(ag_coninfo_t *out);

void poke(uint16_t x, uint16_t y, char ch, uint8_t attr);
void fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, char ch, uint8_t attr);
```

Консоль — **виртуальный текстовый экран** 80×25, который система одновременно
отдаёт в UART, а позже в telnet и на локальный дисплей. Приложение пишет в
экран, а не в порт.

**`printf`** реализован ядром (`vsnprintf` из newlib), поэтому у приложения не
появляется своих 20 КБ форматирования. Поддерживается всё, что есть в полном
newlib: `%p`, `%llu`, `%f` (сборка не использует nano-вариант форматирования).
`printf` в ABI — обычная функция с `...`, обёртка `ag_printf` — макрос.

**Один вызов ограничен 255 символами.** Буфер форматирования у ядра один и на
стеке; что не влезло, обрезается, а возвращается при этом то, что *было бы*
написано — как и положено `snprintf`. Строка, которая может оказаться длиннее,
печатается по частям, а не одним вызовом.

**`getch`** блокируется до символа и возвращает его; `< 0` — ошибка. Клавиши без
символа (стрелки, F1..F12) через `getch` **не приходят** — для них есть
[`inp`](#inp--ввод-событиями).

**Клавиатура принадлежит переднему плану.** Фоновый процесс, спросивший ввод,
получает `-AG_EPERM` немедленно, а не ждёт того, чего не будет. То же для
`readline`; `kbhit` в фоне отвечает `0`.

**`kbhit`** отвечает «есть ли что-то во вводе» (0/1), но **не** «сколько»:
подсмотреть без изъятия нынешняя консоль не умеет.

**`readline`** читает строку с элементарным редактированием и возвращает длину;
`< 0` — отказ или отмена (`Ctrl+C`).

**`poke`/`fill`** пишут прямо в ячейки экрана, не двигая курсор и не вызывая
прокрутку — это то, чем рисуют рамки и панели. Именно поэтому вывод `printf`
в правый нижний угол — ошибка: он прокручивает экран, а `poke` нет.

**Атрибут** — байт в раскладке CGA: `AG_ATTR(fg, bg)`, цвета из `ag_color`
(`AG_BLACK`..`AG_WHITE`, 16 значений). Обёртка `ag_color(fg, bg)` собирает
атрибут сама.

**`codepage`/`set_codepage`** — что значит байт в ячейке. Ячейка хранит один
байт, как видеопамять PC, и страница решает, какой это символ: 437 (рисование
рамок, кириллицы нет), 866 (кириллица MS-DOS, рамки на тех же местах, что в 437)
или 1251 (кириллица Windows, рамок нет). `set_codepage` возвращает `-AG_EINVAL`
на незнакомый номер и **не переписывает** то, что уже на экране — те же байты
просто начинают значить другое.

Приложение, которое рисует что-то кроме ASCII, обязано спросить: байт русской
буквы в 866 и в 1251 разный, а в 437 её нет вовсе.

```c
if (ag_codepage() == 866) {
    ag_poke(x, y, (char)0xa4, attr);   /* д */
}
```

Перевод в UTF-8 для терминала делает система, на выходе. Приложение про UTF-8 не
знает ничего — и в этом смысл: столбцы считаются в ячейках, а не в байтах.

`info` заполняет `ag_coninfo_t`: размер, положение курсора, текущий атрибут и
`has_local_display` (`true`, когда soft/`fb0` или будущая панель подняты).

## `gfx` — графика

```c
ag_err_t acquire(ag_gfxinfo_t *out);
void     release(void);
void     flush(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void     swap(void);
void     clear(uint32_t color);          /* 0x00RRGGBB */
void     fill_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t color);
void     blit(...);
int32_t  text(int16_t x, int16_t y, const char *s, uint32_t fg, uint32_t bg);
void     backlight(uint8_t percent);    /* no-op на soft */

/* Soft-draw (ABI 0.9), integer raster, clip to framebuffer */
void     pixel(int16_t x, int16_t y, uint32_t color);
void     line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color);
void     circle(int16_t cx, int16_t cy, uint16_t r, uint32_t color);
void     fill_circle(int16_t cx, int16_t cy, uint16_t r, uint32_t color);
void     poly_begin(void);
ag_err_t poly_vertex(int16_t x, int16_t y);
void     poly_fill(uint32_t color);      /* convex, ≥3 verts */
void     poly_stroke(uint32_t color);
void     fill_convex(const ag_point_t *pts, int32_t n, uint32_t color);
void     stroke_convex(const ag_point_t *pts, int32_t n, uint32_t color);

/* ABI 0.16 */
void     clip(int16_t x, int16_t y, uint16_t w, uint16_t h);
void     clip_reset(void);               /* whole framebuffer again */
void     stroke_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t color);
void     fill_round_rect(int16_t x, int16_t y, uint16_t w, uint16_t h,
                         uint16_t r, uint32_t color);

/* ABI 0.17 — chroma blit; soft path RGB565 / RGB565_BE */
void     blit_key(int16_t x, int16_t y, uint16_t w, uint16_t h,
                  const void *src, uint32_t src_stride, ag_pixfmt_t fmt,
                  uint32_t key_rgb);
/* Stateful RGB565 LE path for Argon CC (≤6 call args): bind → copy/keyed */
void     blit_bind(const void *src, uint32_t src_stride);
void     blit_copy(int16_t x, int16_t y, uint16_t w, uint16_t h);
void     blit_keyed(int16_t x, int16_t y, uint16_t w, uint16_t h,
                    uint32_t key_rgb);

/* ABI 0.25 — clip 8×16 text to `w` pixels; overflow becomes "..." */
int32_t  text_fit(int16_t x, int16_t y, uint16_t w, const char *s,
                  uint32_t fg, uint32_t bg);

/* ABI 0.26 — nearest RGB565 on the buffer from blit_bind */
void     blit_src_rect(int16_t sx, int16_t sy, uint16_t sw, uint16_t sh);
void     blit_scaled(int16_t dx, int16_t dy, uint16_t dw, uint16_t dh);
void     blit_tiled(int16_t dx, int16_t dy, uint16_t dw, uint16_t dh);
void     poly_uv(int16_t u, int16_t v);   /* source pixels; next poly_vertex */
void     poly_fill_tex(void);             /* bilinear UV on quads; else affine */
```

Сейчас бэкенд — **программный** RGB565 framebuffer (`d:\fb0`, по умолчанию
640×400). Front — то, что видит QEMU/`fb0`/консоль; при `acquire`, если есть
PSRAM, приложение рисует в **back** (`double_buf = true`, `direct = false`).
`flush` и `swap` копируют draw→front и делают present (без busy-wait на ENA).
`flush` **уважает прямоугольник**: копируется только он, и для эмулятора это
разница между 140 КБ и 500 КБ процессорного копирования на кадр. На экран при
этом обновляются целые строки прямоугольника — столбцы вне него берутся из
front, то есть остаются какими были, но панели отдаётся непрерывный участок
кадра (иначе QEMU красит область не теми пикселями; разбор —
в [`07-emulator-performance.md`](../07-emulator-performance.md)).
Нулевые `w`/`h` означают «весь кадр», как и `swap`.
`clear` чистит **и** front, то есть экран целиком: приложение, которое дальше
показывает только свой прямоугольник, не остаётся в рамке из остатков консоли.
Без back-буфера (`direct = true`) рисуют прямо во front. Текстовый blit на fb
на время захвата останавливается; `release` презентует и возвращает текст.
Цвета — `0x00RRGGBB`. Шрифт ядра — 8×16. `text(..., bg=AG_GFX_TRANS)` рисует
только «включённые» биты глифа (подписи поверх скина). `blit` с
`AG_PIX_ARGB8888` блендит packed LE `B,G,R,A` на RGB565 (a=0 пропуск, a=255
замена).
Soft-draw (`pixel`/`line`/`circle`/`poly_*`) — целочисленный растеризатор в
[`draw.c`](../../components/argon_kernel/src/dev/draw.c); полигоны только
**выпуклые**. ABI 0.16 добавляет `clip` / `stroke_rect` / `fill_round_rect`.
ABI 0.17 — `blit_key` и stateful `blit_bind` / `blit_copy` / `blit_keyed`
(chroma-key спрайты; игровой tilemap/sprite слой — Mini-C
библиотека [`apps/cc/lib/g2d`](../../apps/cc/lib/g2d), не ядро).
ABI 0.24 — прозрачный текст и ARGB blit, без новых слотов vtable.
ABI 0.25 — `text_fit` (обрезка строки по ширине с `"..."`).
ABI 0.26 — `blit_src_rect` / `blit_scaled` / `blit_tiled` (nearest RGB565) и
`poly_uv` / `poly_fill_tex` (билинейные UV на четырёхугольнике, иначе
аффинный веер треугольников).
Полноценный GUI toolkit — не в ядре (LVGL при необходимости линкуется в
host-`.AXE`, см. [`06-ideas.md`](../06-ideas.md) §3.4).

Без дисплея (`display.driver = none`) `acquire` даёт `-AG_ENODEV`. Флаг
образа `AG_AXE_NEEDS_GFX` отказывает в запуске, если дисплея нет.

Проверка в QEMU: `apps/gfxdemo` (rects + line/circle/poly), затем
`gfxdump t:\shot.ppm`. Игра на Argon CC: `apps/cc/examples/asteroids.c`.

## `inp` — ввод событиями

```c
bool     poll(ag_event_t *out, uint32_t timeout_ms);
void     flush(void);
bool     key_pressed(uint16_t keycode);   /* sticky serial; drains queue */
uint16_t mods(void);
uint32_t pad(int which);   /* 0=pad0, 1=pad1, 2=sys, 3=pad0hi, 4=pad1hi */
int32_t  btn(int id);      /* AG_BTN_* on pad 0; live pad, else sticky */
int32_t  btnp(int pad, int id); /* ABI 0.11: same on pad 0 or 1 */
bool     inject(const ag_event_t *ev); /* ABI 0.18: push into poll queue */
```

Целые события — то, что нужно приложению, рисующему свой экран: у стрелок и
F-клавиш нет символа, и `con->getch` их передать не может. ABI 0.18
`inject` — для драйверов вроде [`MOUSEVIRT.SYS`](../../apps/mousevirt)
(`POINTER` / `WHEEL` → тот же `poll`).

**Игры / удержание / аккорды (SMS, MD, Asteroids):** не `key_pressed` по UART.
Нужен слой ввода (`/dev/joy0`, `inp->pad` / `btn` / `btnp`), который сегодня
кормит HostFS PADPUSH (`argon run -HostFs`, `sms.cfg`). Снимок — 6 байт
(pad0, pad1, sys, pad0hi, pad1hi, ver); первые три совместимы со старым
3-байтовым хостом. Level-state ~60 Гц: диагонали, A+B+C и Start держатся.
`H:\sms.pad` — тот же кэш как файл совместимости. `key_pressed` — только
fallback без HostFS. Кнопки: `AG_BTN_UP..B2`, `PAUSE`, `QUIT`, плюс
`C` / `START` / `X` / `Y` / `Z` / `MODE` (высокие биты).

**`poll`** возвращает `true`, если событие получено. `timeout_ms`: `0` — только
опрос, `UINT32_MAX` — ждать сколько угодно. В фоновом процессе — всегда `false`.

`ag_event_t` — тип, метка времени и объединение по типу. Для клавиатуры:
`key.keycode` — **USB HID usage id** (см. [`argon/keys.h`](../../sdk/include/argon/keys.h)),
то есть физическая клавиша; `key.unicode` — символ, который она дала, или `0`,
если не дала; `key.mods` — набор из `ag_keymod`; `key.repeat` — автоповтор.

HID-нумерация выбрана не из любви к стандартам: так USB-клавиатура отдаёт коды
без преобразования, а всё остальное (PS/2, escape-последовательность telnet,
матричная клавиатура) переводится в одно и то же пространство.

**`key_pressed` — `NULL`, и честно:** терминал не сообщает, что клавиша
*удерживается*, он присылает нажатие и молчит. Спрашивать надо через
`AG_HAS(inp, key_pressed)`; вызов появится вместе с USB-клавиатурой.

Типы событий за пределами клавиатуры (`AG_EV_POINTER_*`, `AG_EV_DEVICE_*`,
`AG_EV_MEDIA_*` объявлены, но пока не приходят. `AG_EV_QUIT` приходит при soft-stop
(`Ctrl+C` / `F12`). `AG_EV_FOCUS_LOST` / `GAINED` приходят при смене слота сессии
(ABI 0.20); вне фокуса не вызывайте `flush`/`swap` — есть также `ag_focused()`:
источников для них ещё нет.

## `time` — время

```c
ag_time_t   us(void);
uint32_t    ms(void);
uint64_t    cycles(void);
void        delay_ms(uint32_t ms);
void        delay_us(uint32_t us);
ag_err_t    get_datetime(ag_datetime_t *out);
ag_err_t    set_datetime(const ag_datetime_t *dt);   /* NULL: нужен RTC */
ag_handle_t timer_create(...);                       /* NULL */
ag_err_t    timer_delete(ag_handle_t h);             /* NULL */
```

`us`/`ms` — с момента загрузки, монотонно.

`cycles` — **настоящие такты того ядра, на котором вызвано**
(`esp_cpu_get_cycle_count`), 32 бита, переполняется примерно раз в 18 с на
240 МГц: считайте разности и держите окно коротким. До 16 августа 2026 запись
отдавала то же, что `us`, — то есть была бесполезна ровно для того, ради чего
существует: микросекунда на 240 МГц это 240 тактов, а весь отсчёт при 48 кГц —
5000.

**В QEMU этот счётчик не считает работу.** Эмулятор ведёт его от виртуальных
часов с частотой ядра 40 МГц, а виртуальные часы следуют за временем хоста, так
что «такты» там — это микросекунды, умноженные на сорок. Мерить в эмуляторе
стоимость кода надо через `argon test -Icount`, где одна выполненная инструкция
продвигает время ровно на наносекунду и `us() * 1000` становится точным счётом
инструкций. Подробности —
[`docs/08-circuit-simulation.md`](../08-circuit-simulation.md).

`delay_ms` уступает процессор (`vTaskDelay`); `delay_us` — активное ожидание,
потому что ниже тика уступать нечему. Для паузы в миллисекундах и больше нужен
`delay_ms`: `delay_us(100000)` — это 100 мс, отнятые у всей системы.

`get_datetime` отдаёт UTC из системных часов. Без RTC и без сети они начинаются
с нуля при каждой загрузке — по ним нельзя судить о календарной дате.
`set_datetime` — `NULL` до появления драйвера RTC.

## `task` — потоки

```c
ag_thread_t create(void (*fn)(void *), void *arg, const char *name,
                   size_t stack, int priority, uint32_t flags);
void        exit(void);
ag_err_t    join(ag_thread_t t, uint32_t timeout_ms);
void        yield(void);
void        sleep_ms(uint32_t ms);
ag_thread_t self(void);

ag_mutex_t  mutex_create(void);   void mutex_delete(ag_mutex_t m);
bool        mutex_lock(ag_mutex_t m, uint32_t timeout_ms);
void        mutex_unlock(ag_mutex_t m);

ag_sem_t    sem_create(uint32_t initial, uint32_t max);  void sem_delete(ag_sem_t s);
bool        sem_take(ag_sem_t s, uint32_t timeout_ms);   void sem_give(ag_sem_t s);

ag_queue_t  queue_create(uint32_t items, size_t item_size);
void        queue_delete(ag_queue_t q);
bool        queue_send(ag_queue_t q, const void *item, uint32_t timeout_ms);
bool        queue_recv(ag_queue_t q, void *item, uint32_t timeout_ms);

void        critical_enter(void);  void critical_exit(void);
```

Потоки принадлежат процессу: что он не убрал сам, система останавливает при
завершении. Это настоящие задачи FreeRTOS — вытесняющие, с приоритетами, и
каждая стоит стека **из internal SRAM**, самой дефицитной памяти. Отсюда предел:
**четыре потока на процесс**.

`create` возвращает `NULL`, если поток создать не удалось. `flags` —
`AG_THREAD_APP_CORE` (по умолчанию), `AG_THREAD_SYS_CORE`, `AG_THREAD_ANY_CORE`.
Приоритет — числом, как в FreeRTOS: больше — важнее.

`join` ждёт завершения с таймаутом, `-AG_ETIMEDOUT` если не дождался. Замки,
семафоры и очереди — обёртки над FreeRTOS: `mutex_lock` возвращает `false` по
таймауту, `queue_send`/`queue_recv` — тоже.

`critical_enter`/`critical_exit` запрещают вытеснение на текущем ядре. Держать
надо микросекунды: пока критическая секция открыта, на этом ядре не работает
ничего, включая обработчики.

Одно правило, стоящее отдельного упоминания: **задачу, держащую мьютекс, нельзя
снять**. Легального способа отобрать мьютекс у удалённой задачи в FreeRTOS нет,
поэтому супервизор сначала спрашивает, у кого замок, ждёт до 500 мс и, если он
не отпущен, **отказывается** снимать процесс. Приложение, надолго берущее замок,
рискует оказаться неснимаемым.

## `proc` — процессы

```c
int32_t  exec(const char *path, int argc, const char **argv);
ag_pid_t spawn(const char *path, int argc, const char **argv, uint32_t flags);
ag_err_t wait(ag_pid_t pid, int32_t *exit_code, uint32_t timeout_ms);
ag_err_t kill(ag_pid_t pid);
ag_pid_t self(void);
ag_err_t enumerate(uint32_t index, ag_procinfo_t *out);
ag_err_t foreground(ag_pid_t pid);
const char *getenv(const char *key);            /* NULL: окружения нет */
ag_err_t    setenv(const char *key, const char *value);  /* NULL */
bool     interrupted(void);
bool     focused(void);      /* ABI 0.20: session slot has input/display focus */
void     watchdog(uint32_t ms);
```

**`exec`** загружает, запускает и ждёт; возвращает **код возврата** дочернего
процесса, а при неудаче загрузки — отрицательный код ошибки. Различать их — дело
приложения: код возврата тоже может быть отрицательным, поэтому договор такой,
какой есть, и `exec` стоит проверять на конкретные ошибки загрузки.

**`spawn`** возвращает `pid > 0` или отрицательный код. Флаги из
`ag_spawn_flags`: `AG_SPAWN_BACKGROUND`, `AG_SPAWN_RESIDENT` (задел под TSR),
`AG_SPAWN_NO_CONSOLE`. Больше четырёх процессов одновременно не бывает.

**`wait`** возвращает `AG_OK` и код возврата, `-AG_ETIMEDOUT` по таймауту или
`-AG_EKILLED`, если процесс сняли.

**`enumerate`** перечисляет процессы с индекса 0 до `-AG_ENOENT`, заполняя
`ag_procinfo_t`: pid, имя, состояние, признак переднего плана, занятая память,
грубая доля процессора в промилле, время запуска. Это то, что печатает `ps`.

**`interrupted`** — «систему попросили тебя остановить»: `Ctrl+C` или другой
процесс, попросивший вежливо. **Чтение сбрасывает признак.** Приложение,
проверяющее его между порциями работы, можно остановить и дать ему убраться за
собой; не проверяющее — только снять, потеряв то, что было в процессе.

**`watchdog(ms)`** — заявка: «я буду вызывать `sys->heartbeat()` не реже, чем раз
в `ms`; если перестану — снимите меня». `0` снимает заявку. Только по заявке, и
это принципиально: срок, назначенный системой, был бы неверен для любого
приложения, которое законно ждёт долго — карту, ответ, нажатие, — а быть снятым
за ожидание хуже, чем не быть под присмотром.

`getenv`/`setenv` — `NULL`: окружения в системе пока нет.

## `dev` — устройства

```c
ag_err_t    enumerate(uint32_t index, ag_dev_class_t filter, ag_devinfo_t *out);
ag_handle_t open(const char *name);
ag_err_t    close(ag_handle_t h);
int32_t     read(ag_handle_t h, void *buf, size_t len);
int32_t     write(ag_handle_t h, const void *buf, size_t len);
ag_err_t    ioctl(ag_handle_t h, uint32_t cmd, void *arg, size_t arglen);
const void *ops(ag_handle_t h);
ag_err_t    add(const ag_dev_add_t *desc);   /* 0.6, только из ag_driver_init */
ag_err_t    remove(const char *name);        /* 0.6, только из ag_driver_init */
void       *get_priv(ag_device_t *dev);      /* 0.6 */
const ag_probe_hint_t *probe_hint(void);     /* 0.7, только из ag_driver_init */
```

**Устройство — это файл.** Дескриптор, который отдаёт `dev->open`, — обычный
дескриптор: `fs->read`, `fs->write`, `fs->seek` и `fs->close` работают на нём
так же, как на файле, и `fs->open("D:\\sd0", ...)` открывает то же самое.
Так сделано не ради красоты: таблица дескрипторов, учёт владельца и закрытие
того, что осталось от снятого процесса, уже есть в файловой системе, а вторая
такая таблица была бы вторым местом, где про это забывают.

Отсюда следствие, на которое можно опираться: **устройство, открытое процессом,
закрывается вместе с процессом**, снят он или вышел сам.

**Положение принадлежит дескриптору, а не устройству.** Два открытия одного
диска читают с двух разных мест. Устройство-поток (консоль, `null`) смещение
игнорирует; устройство с длиной (диск, раздел flash) им пользуется, и
`AG_SEEK_END` для него — ёмкость.

**`open`** принимает и голое имя (`"sd0"`), и путь (`"D:\\sd0"`, `"/dev/sd0"`).
Открывает на чтение и запись, а устройство «только чтение» открывается на
чтение — то есть отдельного вызова для этого не нужно. `-AG_ENOENT` — нет
такого устройства, `-AG_EBUSY` — устройство эксклюзивно и уже занято.

**`enumerate`** перечисляет с индекса 0 до `-AG_ENOENT`, заполняя `ag_devinfo_t`
(имя, драйвер, класс, флаги). `filter` — класс или `AG_DEV_ANY`; фильтр
перенумеровывает, так что «дай первый диск» — это `enumerate(0,
AG_DEV_STORAGE, ...)`, а не проход по всему списку. Так приложение находит
нужное, **не зная, как это назвали на конкретной плате**.

Классы: `AG_DEV_BUS`, `BLOCK`, `CHAR`, `DISPLAY`, `INPUT`, `SENSOR`, `NET`,
`GPIO`, `AUDIO`, `STORAGE`, `MOTOR`.

Флаги в `ag_devinfo_t.flags`:

| Флаг | Что значит |
|---|---|
| `AG_DEVF_EXCLUSIVE` | один держатель; второе открытие — `-AG_EBUSY` |
| `AG_DEVF_READONLY` | открытие на запись — `-AG_EROFS` |
| `AG_DEVF_HOTPLUG` | может исчезнуть, пока открыто |
| `AG_DEVF_DMA` | обмен можно вести прямо в DMA-память |
| `AG_DEVF_BUSY` | сейчас кем-то открыто (только в ответе `enumerate`) |

**`ioctl`** — команда устройству. Номер команды содержит класс в старшей
половине (`AG_IOC(cls, nr)`), чтобы команда, посланная не туда, отказала, а не
означала там что-то другое. Общие для всех: `AG_IOC_INFO` (заполняет
`ag_devinfo_t` — «что я держу», не возвращаясь к имени), `AG_IOC_RESET`,
`AG_IOC_FLUSH`. Для класса `STORAGE`: `AG_IOC_GEOMETRY` заполняет
`ag_geometry_t` (`sector_size`, `sectors`). Команда, которой устройство не
знает, — `-AG_ENOTSUP`, и это законный ответ, а не отказ.

**`ops`** отдаёт таблицу операций класса — то, чего у файла нет: для дисплея это
будет `flush`, для сенсора — чтение канала. Пока `NULL` у всех: ни одного класса
с собственной таблицей ещё нет.

**Исчезновение.** Устройство, которого не стало (карту вынули, модуль
выгрузили), пропадает из перечисления сразу, а держатель дескриптора узнаёт об
этом на следующем вызове: `read`, `write` и `ioctl` отвечают `-AG_ENODEV`.
Ждать, пока держатель закроет, система не будет — он может больше никогда не
получить управление. `close` при этом успешен: закрывать надо всё равно.

Что есть в этой сборке: `null`, `zero`, `con` (консоль как файл: чтение —
строка с эхом, `Ctrl+Z` — конец ввода), `flash0` (раздел внутреннего flash,
только чтение), `sd0` (карта посекторно, только чтение, съёмная). Оба
хранилища — только чтение намеренно: над ними смонтирована файловая система со
своим кэшем, и запись под ней испортила бы то, что она считает записанным.

Полный пример потребления — `apps/devs/devs.c`; он же проверяет перечисленное
и возвращает код, так что `run` + `errorlevel` — это тест.

**`add` / `remove` / `get_priv` (0.6)** — для загружаемых `.SYS`. `add`
публикует устройство; владелец — модуль, который сейчас в `ag_driver_init`,
поэтому `drv unload` отзывает его устройства. Вне `ag_driver_init` оба
отвечают `-AG_EPERM`. `get_priv` возвращает указатель, переданный в `add`, —
им пользуются колбэки `ag_dev_ops_t`.

**`probe_hint` (0.7)** — при загрузке через `modules.probe` / `drv probe`
возвращает шину и адрес, которые совпали (и поля ID, если они были в строке).
При обычном `drv load` — `NULL`. Подробности — [`05-drivers.md`](05-drivers.md).

## `io` — железо напрямую

```c
ag_err_t gpio_config(int pin, int mode);
void     gpio_write(int pin, int level);
int      gpio_read(int pin);
ag_err_t gpio_isr(int pin, int edge, ag_isr_fn fn, void *arg);
ag_err_t gpio_isr_clear(int pin);

ag_err_t i2c_write(int bus, uint8_t addr, const void *buf, size_t len, uint32_t ms);
ag_err_t i2c_read (int bus, uint8_t addr, void *buf, size_t len, uint32_t ms);
ag_err_t i2c_wrrd (int bus, uint8_t addr, const void *w, size_t wlen,
                                          void *r, size_t rlen, uint32_t ms);
ag_err_t i2c_probe(int bus, uint8_t addr);

ag_err_t spi_xfer(int bus, int cs, const void *tx, void *rx, size_t len);

int32_t  uart_write(int port, const void *buf, size_t len);
int32_t  uart_read (int port, void *buf, size_t len, uint32_t ms);
ag_err_t uart_config(int port, uint32_t baud, int databits, int parity, int stopbits);

int32_t  adc_read(int channel);                       /* NULL в этой сборке */
ag_err_t pwm_config(int pin, uint32_t freq_hz, uint8_t bits);
ag_err_t pwm_set(int pin, uint32_t duty);
```

Полное доверие, как в DOS: приложение дёргает пин и разговаривает с чипом на
шине, не спрашивая ни у какого драйвера разрешения. Единственное, чего оно
сделать не может, — **забрать то, чем уже пользуются**.

### Правило владения пином

Оно одно, и стоит его прочитать целиком:

* **пин системы** (консоль, карта, flash, PSRAM, а также пины поднятой шины) —
  настроить или записать нельзя, ответ `-AG_EACCES`. Это «никогда», а не
  «попозже»;
* **пин, занятый кем-то другим** — `-AG_EBUSY`. Это «попозже»;
* **читать можно любой пин**, чей угодно. Чтение ничего не меняет, а
  возможность посмотреть на линию, которой пользуется система, — это разница
  между «разобрался с разводкой» и «погадал».

`gpio_config` занимает пин на вызывающий процесс. **Всё, что процесс занял,
возвращается, когда он завершается** — включая снятие обработчика прерывания и
возврат пина в высокоимпедансный вход. Это не удобство: обработчик, оставшийся
на пине после того, как его код освободили, — это плата, которая встанет на
следующем фронте, и в журнале не будет ни строчки о том, почему.

Занятые пины видны командой `io` — с именем того, кто их держит, и зачем.

### GPIO

`mode`: `AG_GPIO_IN`, `AG_GPIO_OUT`, `AG_GPIO_OUT_OD` (открытый сток),
`AG_GPIO_IN_PULLUP`, `AG_GPIO_IN_PULLDOWN`.

**У выхода вход остаётся включённым**, поэтому `gpio_read` на выходе говорит,
что на линии. Для двухтактного выхода это бесплатная проверка записанного; для
открытого стока — единственный способ вообще что-то прочитать, потому что низкий
уровень там означает «кто-то держит», а высокий — «никто не держит». Так читают
I2C и 1-Wire, если крутить их руками.

`gpio_read` возвращает `0`, `1` или `-AG_ERANGE` для номера, которого у чипа нет.
`gpio_write` ничего не делает молча, если пин не ваш, — проверяйте владение по
успеху `gpio_config`.

### Прерывания

`gpio_isr(pin, edge, fn, arg)` требует, чтобы пин был **уже занят вызывающим**:
прерывание на пине, который никто не настроил как вход, — просьба, которая пока
ничего не значит. `edge`: `AG_EDGE_RISING`, `AG_EDGE_FALLING`, `AG_EDGE_BOTH`.

Обработчик — это код приложения, и он **имеет право быть обработчиком** ровно
потому, что код образа лежит в арене, то есть во внутренней SRAM; код в PSRAM
обработчиком быть не может. Правила внутри обработчика обычные для прерываний:
коротко, без выделения памяти, без вывода на консоль, без файловых операций.

### I2C

Номер шины — как в `BOARD.CFG`: `0` это `[i2c0]`. Шина поднимается при первом
обращении. Ответы `i2c_probe` различают три разные вещи, и различают
намеренно:

| Ответ | Что это значит |
|---|---|
| `AG_OK` | по этому адресу кто-то есть |
| `-AG_ENOENT` | шина работает, по этому адресу никого |
| `-AG_ENODEV` | такой шины нет — не описана в `BOARD.CFG` |
| прочее | шина не работает |

Сваливать первые два в один код нельзя: скан спрашивает про 112 адресов, и тогда
«шины нет» выглядит ровно как «шина пустая».

### SPI

Номер шины — номер из даташита: `2` это SPI2. `cs` — пин выбора кристалла,
которым управляет контроллер, или `-1`, если приложение держит его само.

**Одна передача — не больше 1024 байт**, `-AG_EINVAL` сверх того. Причина
названа честно: данные приложения лежат в PSRAM, а DMA хочет внутреннюю память,
поэтому каждая передача идёт через промежуточный буфер, и предел — это его
размер. Кому нужно больше, тот делит; кристаллу, который не переживёт снятия
`CS` между частями, нужен `cs = -1` и собственный выбор кристалла.

### UART

Порт `0` — консоль, и он **не выдаётся**: `-AG_EBUSY`. Порты `1` и `2` берут
пины и скорость из `BOARD.CFG` и поднимаются при первом обращении;
`uart_config` меняет параметры. `parity`: `0` нет, `1` нечёт, `2` чёт.

`uart_read` ждёт до `ms` и возвращает, сколько байт пришло — **короткое чтение
законно**, как и у файлов.

### ADC и PWM

`adc_read` в стандартной сборке **`NULL`** — спрашивайте `AG_HAS(io, adc_read)`.
Причина в `CONFIG_ARGON_ENABLE_ADC`: драйвер ADC из ESP-IDF ставит конструктор,
который выполняется до `app_main` и читает калибровку аналогового тракта, а QEMU
её не моделирует и загрузка на этом останавливается. Система проверяется в QEMU,
поэтому по умолчанию ADC не линкуется; для реальной платы опция включается.

`pwm_config(pin, freq_hz, bits)` занимает пин и канал LEDC (их восемь);
`pwm_set(pin, duty)` задаёт заполнение, `0..(1 << bits) - 1`, со срезкой сверху.
Таймеров у чипа четыре, поэтому у пятого и дальше каналов частота общая с
кем-то — предел, о котором лучше знать, чем обнаружить.

### Чего в `io` нет

`probe` по таблице ID для I2C есть (`modules.probe`, `drv probe`). Нет:
1-Wire, TWAI/CAN, RMT, MCPWM, I2S, probe по SPI-панелям.
Всё это фаза 4 целиком; здесь то, без чего нельзя написать драйвер чипа на шине.

## `power` — частота, экран и оповещение

Появилась в **0.34**. Три вызова, и ни один из них ничего не решает за
приложение.

```c
static inline ag_err_t ag_power_status(ag_power_status_t *out);
static inline ag_err_t ag_power_answer(ag_power_answer_t answer, const char *why);
static inline ag_err_t ag_power_hold(bool on, const char *why);
```

Система не меняет частоту и не гасит экран молча: сначала она объявляет, что
собирается сделать, и ждёт до полусекунды. Приложение узнаёт об этом из
`status()` — `pending != mode`, или `pending_cpu_max_mhz` меньше текущей
частоты — и отвечает одним из трёх:

| Ответ | Значение |
|---|---|
| `AG_POWER_OK` | работаю дальше, мне всё равно |
| `AG_POWER_PARKED` | остановился сам; разбудите обратным переходом |
| `AG_POWER_HOLD` | так работать не могу, вот причина |

`AG_POWER_HOLD` отказывает **команде**, а не системе: человек за консолью может
набрать `power eco /force`, и тогда частота упадёт всё равно — но он увидит, чью
заявку перебивает. Заявку на постоянно (`hold(true, …)`) имеет смысл брать
realtime-тракту, который не станет опрашивать статус каждый кадр; она снимается
вместе с процессом, как любой другой ресурс.

Молчание — согласие. Приложение, которое не спрашивает статус, работает как
раньше; переход происходит без него, и в отчёте `power` видно, что оно не
отвечало. Это сделано намеренно: одно зависшее приложение не должно означать
разряженную батарею.

```c
ag_power_status_t p;
if (ag_power_status(&p) == AG_OK && p.pending_cpu_max_mhz < need_mhz) {
    ag_power_answer(AG_POWER_HOLD, "22 kHz tract");
}
```

`cpu_mhz` в структуре — живое чтение с машины, а не то, что было задано; всё
остальное — настройки. Пример на все четыре поведения —
[`apps/pwr/pwr.c`](../../apps/pwr/pwr.c), разбор и цена —
[`11-power.md`](../11-power.md).

## Чего нет вообще

Не «пока не реализовано», а не предусмотрено моделью:

* **изоляции адресных пространств.** MMU не используется; дикий указатель портит
  чужую память. Это плата за нативную скорость и прямой доступ к железу;
* **fork.** Процесс создаётся только загрузкой образа;
* **сигналов.** Есть `interrupted` и снятие процесса, и только;
* **пайпов и перенаправления между процессами.** Перенаправление вывода в файл
  умеет шелл, но это его дело, а не ABI;
* **динамической загрузки библиотек приложением.** `.SYS` — драйверы, их
  загружает система;
* **потоков сверх четырёх и процессов сверх четырёх** — обе цифры про internal
  SRAM, а не про архитектуру.
