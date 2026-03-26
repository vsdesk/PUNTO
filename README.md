# Punto Switcher for Linux/Wayland

Автоматическое переключение раскладки **ru↔en** в стиле Punto Switcher для
**Wayland** через **`punto-switcher-daemon`** (evdev/uinput) и GUI
`punto-switcher-config`.

### Кому не подходит

Если вам нельзя запускать daemon с доступом к `input/uinput`, этот репозиторий
не подойдёт. Текущая рабочая реализация — именно `punto-switcher-daemon`
(глобальные hotkeys и автосвитч через evdev/uinput), а не только IME-модуль.

---

## Архитектурные решения (зафиксированы)

| Аспект | Выбор | Обоснование |
|--------|-------|-------------|
| Input path | **daemon (evdev/uinput)** + fallback clipboard paste | Глобальные hotkeys/autoswitch в Wayland-приложениях без зависимости от surroundingText |
| Qt версия | **Qt6 Widgets** | Доступен в Ubuntu 22.04+, Debian 12+, Fedora 37+; один dev-пакет |
| Packaging deb | **debhelper 13** + `dpkg-buildpackage` | Нативный подход, корректный `${shlibs:Depends}` |
| Packaging rpm | **rpmbuild** + `.spec` | Стандарт для Fedora/RHEL |
| Build system | **CMake 3.20+** | Интегрируется с Qt6 и системными libevdev/xkbcommon |
| Testing | **GTest** | Стандарт для C++ |
| Auto-switch | **Bigram-scoring heuristic** | Нет ML, нет словаря; top-50 биграм на язык |

---

## Структура репозитория

```
punto-switcher/
├── src/
│   ├── core/                   # Переносимая логика (без daemon/Qt зависимостей)
│   │   ├── CharMapping.h/.cpp  # Таблица JCUKEN↔QWERTY + char-level swap
│   │   ├── WordSwapper.h/.cpp  # Punto-style swapLastWord / swapSelection
│   │   ├── AutoSwitchHeuristic.h/.cpp  # Детектор "неверной раскладки"
│   │   └── Utf8Utils.h         # UTF-8↔UTF-32 без внешних зависимостей
│   ├── daemon/                 # Текущий runtime: evdev/uinput daemon
│   │   ├── PuntoDaemon.h/.cpp  # Основная логика hotkeys/autoswitch/inject
│   │   ├── EvdevUinput.h/.cpp  # Захват клавиатур + uinput forward/inject
│   │   ├── LayoutController.*  # Синхронизация раскладки с KDE
│   │   └── DaemonConfig.*      # ~/.config/punto-switcher/daemon.ini
│   └── gui/                    # Qt6 конфигуратор
│       ├── main.cpp
│       ├── MainWindow.h/.cpp   # Главное окно (tabs: Hotkeys / Auto-Switch / Debug)
│       ├── HotkeyEditor.h/.cpp # Виджет записи клавиатурного сочетания
│       └── punto-switcher-config.desktop.in
├── tests/                      # GTest unit-тесты
│   ├── test_mapping.cpp        # CharMapping: все 33 символа, регистры, round-trip
│   ├── test_swapper.cpp        # WordSwapper: границы слов, selection, round-trip
│   └── test_heuristic.cpp      # AutoSwitch: false-pos/neg, guards, bigram score
├── packaging/
│   ├── deb/                    # debhelper 13 debian/ tree
│   │   ├── control, rules, changelog, copyright, compat
│   │   └── punto-switcher.install
│   └── rpm/
│       └── punto-switcher.spec # rpmbuild spec
├── docker/
│   ├── Dockerfile.deb          # Ubuntu 24.04 builder → *.deb
│   └── Dockerfile.rpm          # Fedora 41 builder → *.rpm
└── .github/workflows/
    └── build-packages.yml      # CI: tests + deb + rpm + GitHub Release
```

---

## Функции

### A. Swap последнего слова (default: `Alt+'`)

Определяет "последнее слово" как последовательность символов до ближайшей
левой границы слова (пробел, пунктуация, перевод строки). Гифен `-` и
апостроф `'` считаются частью слова (аналогично Punto).

**Алгоритм (daemon):**
1. Ведёт `wordBuffer` из реальных key events.
2. На hotkey удаляет слово через backspace (по codepoint count).
3. Вставляет swapped-текст через `wtype` (или fallback `wl-copy + Ctrl+V`).
4. Обновляет `lastWord`, чтобы повторное нажатие делало обратный swap (round-trip).

### B. Swap выделенного текста (default: `Alt+Shift+'`)

1. Делает `Ctrl+C` и читает clipboard (`wl-paste -n`).
2. Если есть однотокенное выделение — вставляет swapped.
3. Если выделения нет/не поддерживается — fallback к `swap_last`.

**Ограничения selection под Wayland:**

| Toolkit | Статус |
|---------|--------|
| Qt 5/6 (KDE, Qt apps) | ✅ работает (text-input-v3 surrounding_text) |
| GTK4 | ✅ работает |
| GTK3 (XWayland) | ⚠️ может не предоставлять anchor ≠ cursor |
| Electron (Wayland native) | ⚠️ частично; зависит от версии |
| Electron (XWayland mode) | ⚠️ зависит от поведения clipboard/hotkeys |
| Konsole / terminal emulators | ⚠️ часто не реализуют text-input surrounding_text |

При отсутствии selection срабатывает fallback `swap_last`.

### C. Auto-switch (default: `Alt+Shift+A`)

Автозамена срабатывает **только на границе слова** (space/enter/punctuation):

```
Пользователь набрал: "ghbdtn " (= "привет" на EN-раскладке)
                            ^--- граница слова
→ daemon видит boundary key
→ проверяет слово "ghbdtn"
→ bigramScore("ghbdtn", EN) = 0   (нет EN биграм)
   bigramScore("привет", RU) = X  (есть RU биграм)
→ разница > threshold → swap
→ итого в поле: "привет "
```

**Анти-дерганье:** после успешного auto-switch токен помечается как `frozen`;
новые символы → новый токен → `frozen` снимается.

### D. Таблица символов JCUKEN ↔ QWERTY

Стандартная российская раскладка (33 буквы + Ё):

| Рус | En | | Рус | En |
|-----|----|-|-----|-----|
| й   | q  | | а   | f   |
| ц   | w  | | п   | g   |
| у   | e  | | р   | h   |
| к   | r  | | о   | j   |
| е   | t  | | л   | k   |
| н   | y  | | д   | l   |
| г   | u  | | ж   | ;   |
| ш   | i  | | э   | '   |
| щ   | o  | | я   | z   |
| з   | p  | | ч   | x   |
| х   | [  | | с   | c   |
| ъ   | ]  | | м   | v   |
| ф   | a  | | и   | b   |
| ы   | s  | | т   | n   |
| в   | d  | | ь   | m   |
| б   | ,  | | ю   | .   |
| ё   | `` ` `` | | | |

Регистр сохраняется: `Й→Q`, `й→q`, `Ё→`` ` ```.

---

## Hotkeys по умолчанию

| Действие | Hotkey (daemon.ini формат) | Изменяемый |
|----------|------------------------|-----------|
| Swap Last Word | `Alt+apostrophe` | ✅ GUI / config |
| Swap Selection | `Alt+shift+apostrophe` | ✅ GUI / config |
| Toggle Auto-Switch | `Alt+shift+a` | ✅ GUI / config |
| Undo Last Switch | `Alt+shift+BackSpace` | ✅ GUI / config |

**Конфликты:** если системный хоткей перехватывается раньше daemon
(например, глобальный shortcut KWin), действие Punto не сработает.
Проверяйте логи через `journalctl --user | grep -i punto-switcher-daemon`.

---

## Эвристика Auto-Switch (формальное описание)

```
shouldSwitch(word, layout):
  if NOT enabled: return false
  if count(letters in word) < minWordLength: return false
  if contains(digit | '@' | '://'): return false

  swapped = CharMapping::swapWord(word)
  otherLayout = opposite(layout)

  score_orig   = bigramScore(word,    layout)
  score_swapped = bigramScore(swapped, otherLayout)

  return (score_swapped - score_orig) > confidenceThreshold
```

`bigramScore(text, layout)`:  
= (число биграм из `text`, входящих в top-50 биграм `layout`) / (общее число буквенных биграм в `text`)

**Параметры по умолчанию:**
- `minWordLength = 3`
- `confidenceThreshold = 0.15`

**Биграмные модели:** top-50 биграм взяты из открытых корпусов (НКРЯ для RU,
BNC/Wikipedia для EN). Расширение до top-100 и добавление тригармных моделей
запланировано как Итерация 3.

---

## Как собрать

### Требования

**Общие:**
```
cmake >= 3.20, gcc/clang C++17
```

**Ubuntu/Debian:**
```bash
sudo apt install \
  cmake build-essential pkg-config \
  libevdev-dev libxkbcommon-dev \
  qt6-base-dev extra-cmake-modules \
  libgtest-dev
```

**Fedora:**
```bash
sudo dnf install \
  cmake gcc-c++ \
  libevdev-devel libxkbcommon-devel \
  qt6-qtbase-devel extra-cmake-modules gtest-devel
```

### Сборка из исходников

```bash
git clone https://github.com/your-org/punto-switcher
cd punto-switcher

# Конфигурация
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr

# Сборка
cmake --build build -j$(nproc)

# (опционально) Тесты
cmake -B build -DBUILD_TESTING=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Установка (или в staging для пакетирования)
sudo cmake --install build
```

---

## Как собрать .deb (Ubuntu/Debian)

### Локально через Docker

```bash
# Собрать Docker-образ
docker build -f docker/Dockerfile.deb -t punto-deb-builder .

# Запустить сборку → артефакт в ./dist/
mkdir -p dist
docker run --rm -v "$PWD/dist:/dist" punto-deb-builder

# Установить
sudo dpkg -i dist/punto-switcher_1.0.0-1_amd64.deb
sudo apt-get install -f   # устранить зависимости если нужно
```

### Локально без Docker (Ubuntu 24.04)

```bash
# Установить build-deps (см. packaging/deb/control)
sudo apt-get build-dep .

# Создать orig tarball и собрать
tar czf ../punto-switcher_1.0.0.orig.tar.gz ../punto-switcher/
dpkg-buildpackage -us -uc -b -j$(nproc)
# Пакет появится в ../*.deb
```

---

## Как собрать .rpm (Fedora)

### Локально через Docker

```bash
docker build -f docker/Dockerfile.rpm -t punto-rpm-builder .

mkdir -p dist
docker run --rm -v "$PWD/dist:/dist" punto-rpm-builder

# Установить на Fedora
sudo dnf install dist/punto-switcher-1.0.0-1.fc41.x86_64.rpm
```

### Локально на Fedora

```bash
sudo dnf install rpm-build rpmdevtools
rpmdev-setuptree

# Создать исходник
tar czf ~/rpmbuild/SOURCES/punto-switcher-1.0.0.tar.gz \
    --transform 's,^,punto-switcher-1.0.0/,' .
cp packaging/rpm/punto-switcher.spec ~/rpmbuild/SPECS/

rpmbuild -bb ~/rpmbuild/SPECS/punto-switcher.spec
# Пакет в ~/rpmbuild/RPMS/x86_64/
```

---

## Как установить и активировать

### Ubuntu/Debian

```bash
sudo dpkg -i dist/punto-switcher_*_amd64.deb
sudo apt-get -f install -y
```

### Fedora

```bash
sudo dnf install dist/punto-switcher-*.rpm
```

### Запуск daemon

После установки процесс стартует автоматически через desktop autostart.
Проверка:

```bash
pgrep -af punto-switcher-daemon
```

Ручное управление:

```bash
punto-switcher-stop
punto-switcher-daemon
```

---

## Как запустить GUI и где хранится конфиг

```bash
punto-switcher-config
```

Или через меню приложений: **Settings → Punto Switcher Settings**.

**Расположение конфига daemon:**
```
~/.config/punto-switcher/daemon.ini
```

Файл в INI-формате:
```ini
[hotkeys]
swap_last=Alt+apostrophe
swap_selection=Alt+shift+apostrophe
toggle_auto=Alt+shift+a
undo=Alt+shift+BackSpace

[autoswitch]
enabled=true
min_word_length=3
confidence_threshold=0.15
min_plausible_dominant_bigram_score=0.18
```

GUI сохраняет этот файл; daemon подхватывает его при старте
и при переключении `Alt+Shift+A`.

---

## Диагностика daemon

Быстрые команды:

```bash
# процесс жив?
pgrep -af punto-switcher-daemon

# остановить/запустить вручную
punto-switcher-stop
punto-switcher-daemon

# посмотреть последние сообщения daemon
journalctl --user --since "30 min ago" --no-pager | grep -i punto-switcher-daemon
```

Если после deep sleep/Bluetooth reconnect процесс жив, но реакции нет:
- в актуальных версиях daemon сам делает reinit на `ENODEV/EIO/ENXIO` и
  `POLLHUP/POLLERR/POLLNVAL`;
- если не восстановился, перезапусти `punto-switcher-daemon` вручную и
  проверь журнал.

---

## Manual QA Checklist (KDE Wayland)

### Подготовка
- Установить пакет и убедиться, что `punto-switcher-daemon` запущен.
- Открыть Kate или любое Qt-приложение с текстовым полем.

### 1. Swap Last Word
1. Переключиться на EN раскладку.
2. Набрать `ghbdtn` (физические клавиши g-h-b-d-t-n → на EN дают "ghbdtn").
3. Нажать **Alt+'**.
4. ✅ Ожидается: "ghbdtn" → "привет".

### 2. Swap Selection
1. Набрать `hello world`.
2. Выделить слово `hello`.
3. Нажать **Alt+Shift+'**.
4. ✅ Ожидается: "hello" → "руддщ".

### 3. Auto-Switch (EN-layout, Russian word)
1. Держать EN раскладку активной.
2. Набрать `ghbdtn ` (с пробелом в конце).
3. ✅ Ожидается: текст автоматически заменяется на `привет `.

### 4. Anti-flicker при быстром наборе
1. Быстро набрать 15+ символов смешанного текста.
2. ✅ Ожидается: нет хаотичных замен посередине слова.

### 5. Смена hotkey через GUI
1. Открыть `punto-switcher-config`.
2. В "Swap Last Word" нажать новое сочетание, напр. **Ctrl+Space**.
3. Save & Reload.
4. ✅ Ожидается: новый хоткей работает, Alt+' больше не срабатывает.

### 6. Toggle Auto-Switch
1. Нажать **Alt+Shift+A** — авто-свитч отключается.
2. Набрать `ghbdtn ` — замена НЕ происходит.
3. Нажать **Alt+Shift+A** снова — авто-свитч включается.
4. ✅ Всё как ожидается.

### 7. URL-кейс
1. На RU раскладке набрать `реезыЖ..`.
2. ✅ Ожидается: `https://`.

### 8. Проверка случайного UPPERCASE
1. Длительно печатать обычный текст с Alt+' и автосвитчем.
2. ✅ Ожидается: нет внезапного перевода слов в UPPERCASE без нажатого Shift/CapsLock.

### 9. Deep sleep / Bluetooth reconnect
1. Увести ПК в глубокий сон и разбудить.
2. На BT-клавиатуре набрать `ghbdtn `.
3. ✅ Ожидается: автосвитч работает без ручного рестарта daemon.

---

## Известные ограничения

1. **`wtype` может быть недоступен в compositor** — тогда daemon использует
   fallback `wl-copy + Ctrl+V` (для терминалов поддержка ограничена).

2. **Auto-switch без словаря** — эвристика bigram-scoring даёт ~80-90% precision
   на словах длиной ≥5 символов. Короткие слова (3-4 буквы) чаще дают
   false-positive. Можно увеличить `MinWordLength = 5` и `ConfidenceThreshold = 0.25`.

3. **Только пара ru↔en** — намеренное ограничение. Другие раскладки не
   обрабатываются и не должны влиять на логику (отфильтровываются через
   `dominantLayout()`).

4. **Пунктуация ъ/ь/ъ в mapping** — `ъ` → `]`, `ь` → `m`. При наборе
   в EN-раскладке символы `[`, `]`, `;`, `'` интерпретируются как
   Cyrillic. Если пользователь печатает код с этими символами — включить
   режим "только ручной swap" (`AutoSwitch=false`).

5. **Ё** — маппится на `` ` `` (backtick). В большинстве слов Ё корректно
   обрабатывается. В режиме auto-switch: слова с Ё встречаются редко, поэтому
   влияние на false-positive минимально.

6. **Deep sleep / Bluetooth reconnect** — input fd могут отваливаться.
   В актуальной версии daemon делает in-process reinit на `ENODEV/EIO/ENXIO`
   и на poll-события `POLLHUP/POLLERR/POLLNVAL`.

7. **Конфликты хоткеев** — если системный биндинг перехватывает хоткей раньше
   daemon (напр. KWin global shortcut), хоткей Punto не сработает. Решение:
   убрать конфликт в System Settings → Shortcuts, или сменить хоткей в GUI.

---

## Зависимости runtime (точный список)

| Пакет (Ubuntu) | Назначение |
|----------------|-----------|
| `libevdev2` | Чтение input events |
| `libxkbcommon0` | Декод клавиш и раскладки |
| `wl-clipboard` | fallback insert (`wl-copy`/`wl-paste`) |
| `libqt6widgets6` | Qt6 GUI |
| `libqt6core6` | Qt6 core |
| `libqt6gui6` | Qt6 GUI rendering |

Dev-либы (`*-dev`) в рантайм не попадают. `${shlibs:Depends}` в debhelper
автоматически определяет точные символьные зависимости через `ldd`.

---

## CI/CD

GitHub Actions (`.github/workflows/build-packages.yml`):

```
push/PR → unit-tests (Ubuntu 24.04)
         ↓ (if pass)
       ┌─────────────────┬─────────────────┐
       │  build-deb      │  build-rpm      │
       │ (Docker Ubuntu) │ (Docker Fedora) │
       └─────────────────┴─────────────────┘
            ↓ (on tag v*)
         GitHub Release (attaches .deb + .rpm)
```

Артефакты доступны в Actions → Artifacts на 30 дней, и в Releases при тегировании.

---

## Планы развития (Итерации 2 и 3)

**Итерация 2:**
- Trigram scoring для лучшей точности auto-switch.
- Опциональный blacklist слов (исключить "ok", "no", "да", "нет" из auto-switch).
- Системный трей (Qt6 QSystemTrayIcon) для быстрого toggle.

**Итерация 3:**
- Компактный словарь (Bloom filter, 1MB) для проверки плausibility.
- Поддержка history (journal последних N своп-операций с undo).
- Статистика (сколько слов поменяно за сессию).
