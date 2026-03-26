#!/bin/bash
# restore-kde-keyboard-only.sh
#
# Откат к раскладкам только через KDE (без Fcitx5 в трее и без двойных индикаторов).
# Запуск:  bash scripts/restore-kde-keyboard-only.sh
#
# После скрипта: выйти из сеанса и войти снова (или перезагрузка).

set -euo pipefail

HOME="${HOME:-$(getent passwd "$(id -un)" | cut -d: -f6)}"
export HOME

echo "=== Punto / Fcitx5 — откат к клавиатуре только KDE ==="
echo ""

# 1) Встроенный undo из пакета punto-switcher (если установлен)
if command -v punto-switcher-setup &>/dev/null; then
    echo "[1/6] punto-switcher-setup --undo"
    punto-switcher-setup --undo || true
else
    echo "[1/6] punto-switcher-setup не найден — пропуск"
fi

# 2) Остановить Fcitx5
if pgrep -x fcitx5 &>/dev/null; then
    echo "[2/6] Останавливаю fcitx5..."
    pkill -x fcitx5 2>/dev/null || true
    sleep 1
    pkill -9 -x fcitx5 2>/dev/null || true
else
    echo "[2/6] fcitx5 не запущен"
fi

# 3) Убрать переменные окружения IM (Plasma + systemd user)
rm -f "${HOME}/.config/plasma-workspace/env/fcitx5-im.sh" 2>/dev/null || true
rm -f "${HOME}/.config/environment.d/fcitx5-im.conf" 2>/dev/null || true
echo "[3/6] Удалены plasma-workspace/env и environment.d для fcitx5"

# 4) Autostart Punto
rm -f "${HOME}/.config/autostart/punto-switcher-setup.desktop" 2>/dev/null || true
rm -f "${HOME}/.config/autostart/punto-switcher-kde-sync.desktop" 2>/dev/null || true
echo "[4/6] Удалены autostart-файлы Punto"

# 5) KWin: не использовать Fcitx как виртуальную клавиатуру
if command -v kwriteconfig6 &>/dev/null; then
    kwriteconfig6 --file "${HOME}/.config/kwinrc" --group Wayland --key InputMethod "" 2>/dev/null || true
    echo "[5/6] KWin InputMethod очищен"
fi

if command -v qdbus6 &>/dev/null; then
    qdbus6 org.kde.KWin /KWin reconfigure 2>/dev/null || true
fi

# 6) Перечитать раскладки KDE (если доступно)
if command -v qdbus6 &>/dev/null; then
    qdbus6 org.kde.keyboard /Layouts org.kde.keyboard.reloadConfig 2>/dev/null || true
fi

echo ""
echo "Готово."
echo ""
echo "Сделайте вручную (один раз):"
echo "  • Параметры системы → Виртуальная клавиатура — НЕ «Fcitx 5», а встроенная Plasma / отключить."
echo "  • Параметры системы → Запуск и завершение → Автозапуск — отключите «fcitx5», если есть."
echo "  • Параметры системы → Клавиатура → Раскладки — добавьте English (US) и Russian, хоткей переключения (например Alt+Shift или Ctrl+Shift)."
echo "  • Удаление пакета:  sudo apt remove --purge punto-switcher   (по желанию)"
echo ""
echo "Выйдите из сеанса и войдите снова."
echo ""
