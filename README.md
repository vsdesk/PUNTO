# Punto Switcher for Linux/Wayland

Автоматическое переключение раскладки **ru↔en** в стиле Punto Switcher для
**Wayland** (KDE Plasma, GNOME, Sway и других DE) через **Fcitx5** IME.

### Кому не подходит

Если вам нужна **только встроенная клавиатура Plasma** (KDE) **без Fcitx5/IBus** — **этот репозиторий не решает задачу**: перехват и правка текста реализованы **только** как модуль Fcitx5. Параллельно держать «родной» переключатель раскладки KDE и отдельный IME обычно приводит к дублям индикаторов, рассинхрону в браузерах и проблемам с экранной клавиатурой KDE. В таком случае разумный путь — оставить чистый KDE (см. `scripts/restore-kde-keyboard-only.sh`) и не ставить этот пакет; Punto-подобное поведение на Wayland **без** участия IME здесь не предусмотрено.

---

## Архитектурные решения (зафиксированы)

| Аспект | Выбор | Обоснование |
|--------|-------|-------------|
| IME/Engine | **Fcitx5 Module** (не Engine) | Работает поверх любого active input method (русская клавиатура, английская), не требует смены IM |
| Qt версия | **Qt6 Widgets** | Доступен в Ubuntu 22.04+, Debian 12+, Fedora 37+; один dev-пакет |
| Packaging deb | **debhelper 13** + `dpkg-buildpackage` | Нативный подход, корректный `${shlibs:Depends}` |
| Packaging rpm | **rpmbuild** + `.spec` | Стандарт для Fedora/RHEL |
| Build system | **CMake 3.20+** | Интегрируется с Fcitx5CMake и Qt6CMake |
| Testing | **GTest** | Стандарт для C++ |
| Auto-switch | **Bigram-scoring heuristic** | Нет ML, нет словаря; top-50 биграм на язык |

---

## Структура репозитория

```
punto-switcher/
├── src/
│   ├── core/                   # Переносимая логика (без Fcitx5/Qt зависимостей)
│   │   ├── CharMapping.h/.cpp  # Таблица JCUKEN↔QWERTY + char-level swap
│   │   ├── WordSwapper.h/.cpp  # Punto-style swapLastWord / swapSelection
│   │   ├── AutoSwitchHeuristic.h/.cpp  # Детектор "неверной раскладки"
│   │   └── Utf8Utils.h         # UTF-8↔UTF-32 без внешних зависимостей
│   ├── engine/                 # Fcitx5 Module (shared library)
│   │   ├── PuntoModule.h/.cpp  # Главный addon class
│   │   ├── InputTracker.h/.cpp # Состояние per-InputContext
│   │   └── conf/
│   │       └── punto-switcher.conf  # Fcitx5 addon manifest
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

### A. Swap последнего слова (`swap_last_text`, default: `Alt+'`)

Определяет "последнее слово" как последовательность символов до ближайшей
левой границы слова (пробел, пунктуация, перевод строки). Гифен `-` и
апостроф `'` считаются частью слова (аналогично Punto).

**Алгоритм:**
1. Читает `surroundingText()` Fcitx5 (text before cursor).
2. Находит последнее слово назад от курсора.
3. Вызывает `deleteSurroundingText(-N, N)` + `commitString(swapped)`.
4. Сбрасывает `InputTracker` для текущего IC.

**Защита от циклов:** после swap `InputTracker` сбрасывается — повторное
нажатие снова читает surrounding text и делает обратный swap (round-trip).

### B. Swap выделенного текста (`swap_selection`, default: `Alt+Shift+'`)

1. Читает `surroundingText().cursor()` и `.anchor()`.
2. Если `cursor != anchor` — есть выделение: вырезает и вставляет swapped.
3. **Fallback** (cursor == anchor): ведёт себя как `swap_last_text`.

**Ограничения selection под Wayland:**

| Toolkit | Статус |
|---------|--------|
| Qt 5/6 (KDE, Qt apps) | ✅ работает (text-input-v3 surrounding_text) |
| GTK4 | ✅ работает |
| GTK3 (XWayland) | ⚠️ может не предоставлять anchor ≠ cursor |
| Electron (Wayland native) | ⚠️ частично; зависит от версии |
| Electron (XWayland mode) | ❌ X11 не поддерживается нашим модулем |
| Konsole / terminal emulators | ⚠️ часто не реализуют text-input surrounding_text |

При отсутствии selection срабатывает fallback `swap_last`.

### C. Auto-switch (`toggle_auto_switch`, default: `Alt+Shift+A`)

Автозамена срабатывает **только на границе слова** (space/enter/punctuation):

```
Пользователь набрал: "ghbdtn " (= "привет" на EN-раскладке)
                            ^--- граница слова
→ engine видит коммит пробела
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

| Действие | Hotkey (Fcitx5 формат) | Изменяемый |
|----------|------------------------|-----------|
| Swap Last Word | `Alt+apostrophe` | ✅ GUI / config |
| Swap Selection | `Alt+shift+apostrophe` | ✅ GUI / config |
| Toggle Auto-Switch | `Alt+shift+a` | ✅ GUI / config |
| Undo Last Switch | `Alt+shift+BackSpace` | ✅ GUI / config |

**Конфликты:** если Fcitx5 перехватывает хоткей, но целевое приложение
не возвращает управление (e.g. хоткей системный), модуль логирует `WARN`
через `fcitx5_log`. Проверьте вывод `journalctl -t fcitx5`.

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
  libfcitx5core-dev libfcitx5utils-dev libfcitx5config-dev \
  qt6-base-dev extra-cmake-modules \
  libgtest-dev
```

**Fedora:**
```bash
sudo dnf install \
  cmake gcc-c++ \
  fcitx5-devel qt6-qtbase-devel \
  extra-cmake-modules gtest-devel
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
sudo dpkg -i punto-switcher_1.0.0-1_amd64.deb
```

### Fedora

```bash
sudo dnf install punto-switcher-1.0.0-1.fc41.x86_64.rpm
```

### Активация в Fcitx5

#### ⚠️ Обязательно для KDE Wayland

Fcitx5 в KDE Wayland должен запускаться через **KWin** как виртуальная
клавиатура — только тогда он использует нативный Wayland input method interface
(текстовый ввод через compositor, а не через XIM/XWayland).

1. Откройте **«Системные настройки» → «Клавиатура» → «Виртуальная клавиатура»**
   (или найдите "Virtual Keyboard" в поиске настроек).
2. Выберите **«Fcitx 5»** и нажмите «Применить».
3. Выйдите из сессии и войдите снова (или перезапустите KWin).

> Без этого шага Fcitx5 работает через XWayland (XIM), а не через Wayland
> compositor — часть приложений (нативные Wayland) будут игнорировать ввод.
> Подробнее: [fcitx-im.org/wiki/Using_Fcitx_5_on_Wayland#KDE_Plasma](https://fcitx-im.org/wiki/Using_Fcitx_5_on_Wayland#KDE_Plasma)

Также рекомендуется отключить `im-config` (Debian/Ubuntu), если он включён:
```bash
imsettings-switch none   # если установлен imsettings
# или просто убедитесь что в ~/.profile нет строк export GTK_IM_MODULE / QT_IM_MODULE
```

#### Активация модуля

1. Убедиться, что Fcitx5 запущен (в KDE Wayland — он запускается автоматически
   как виртуальная клавиатура после шага выше; иначе вручную):
   ```bash
   fcitx5 -d
   ```

2. Перезапустить Fcitx5 (чтобы подхватил новый модуль):
   ```bash
   fcitx5-remote -r
   # или
   pkill fcitx5; fcitx5 -d
   ```

3. Модуль активируется **автоматически** (тип `Module`, `OnDemand=False`).
   Проверка:
   ```bash
   fcitx5-remote -l | grep punto
   # или
   journalctl --user -t fcitx5 | grep -i punto
   ```

4. **Не нужно** добавлять как отдельный Input Method — модуль работает
   поверх уже настроенных раскладок (русская + английская через fcitx5-keyboard).

---

## Как запустить GUI и где хранится конфиг

```bash
punto-switcher-config
```

Или через меню приложений: **Settings → Punto Switcher Settings**.

**Расположение конфига:**
```
~/.config/fcitx5/conf/punto-switcher.conf
```

Файл в INI-формате, совместимом с Fcitx5:
```ini
SwapLastText=Alt+apostrophe
SwapSelection=Alt+shift+apostrophe
ToggleAutoSwitch=Alt+shift+a
UndoLastSwitch=Alt+shift+BackSpace
AutoSwitch=true
MinWordLength=3
ConfidenceThreshold=0.15
LogLevel=Warning
```

После сохранения GUI автоматически вызывает `fcitx5-remote -r` (hot-reload).
Если fcitx5 не запущен — перезапуск не требуется, конфиг будет прочитан
при следующем старте.

---

## Manual QA Checklist (KDE Wayland)

### Подготовка
- Установить пакет, перезапустить Fcitx5.
- Добавить **English (US)** и **Russian** в Fcitx5 через `fcitx5-config-qt`.
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

---

## Известные ограничения

1. **Selection под Wayland** — работает только если приложение реализует
   `zwp_text_input_v3` с `surrounding_text`. Electron-приложения и некоторые
   GTK3-приложения могут не поддерживать. Fallback: `swap_last_word`.

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

6. **Hot-reload hotkeys** — Fcitx5 Module поддерживает `reloadConfig()`, которая
   вызывается по `fcitx5-remote -r`. GUI делает это автоматически при Save.
   Restart Fcitx5 не нужен.

7. **Конфликты хоткеев** — если системный биндинг перехватывает хоткей раньше
   Fcitx5 (напр. KWin global shortcut), хоткей Punto не сработает. Решение:
   убрать конфликт в System Settings → Shortcuts, или сменить хоткей в GUI.

---

## Зависимости runtime (точный список)

| Пакет (Ubuntu) | Назначение |
|----------------|-----------|
| `libfcitx5core5` / `6` | Fcitx5 runtime (module loading) |
| `libqt6widgets6` | Qt6 GUI |
| `libqt6core6` | Qt6 core |
| `libqt6gui6` | Qt6 GUI rendering |
| `fcitx5` | Daemon |

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
