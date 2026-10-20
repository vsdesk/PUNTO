#include "MainWindow.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QStatusBar>
#include <QLabel>
#include <QFrame>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QFile>
#include <QMenu>
#include <QAction>
#include <QPainter>
#include <QPixmap>
#include <QFont>
#include <QSocketNotifier>
#include <QCloseEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include "DaemonClient.h"
#include "TypoCorrector.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef PUNTO_WITH_FCITX
#error "PUNTO_WITH_FCITX must be defined by CMake (0 or 1)"
#endif

// Default hotkey strings (same format for Fcitx5 and daemon)
static const char* DEFAULT_SWAP_LAST      = "Alt+apostrophe";
static const char* DEFAULT_SWAP_SELECTION = "Alt+shift+apostrophe";
static const char* DEFAULT_TOGGLE_AUTO    = "Alt+shift+a";
static const char* DEFAULT_UNDO_SWITCH  = "Alt+shift+BackSpace";
static const int    DEFAULT_MIN_WORD_LEN  = 3;
static const double DEFAULT_CONFIDENCE    = 0.15;

namespace {

std::string instanceSocketPath() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0])
        return std::string(rt) + "/rocket-switcher-config.sock";
    return std::string("/tmp/rocket-switcher-config.") + std::to_string(getuid()) + ".sock";
}

bool waitSocket(int fd, short events, int timeoutMs) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = events;
    for (;;) {
        const int rc = ::poll(&pfd, 1, timeoutMs);
        if (rc < 0 && errno == EINTR) continue;
        return rc > 0 && (pfd.revents & events);
    }
}

bool notifyExistingInstance() {
    const std::string path = instanceSocketPath();
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    const int conn = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (conn < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return false;
    }
    if (conn < 0 && !waitSocket(fd, POLLOUT, 300)) {
        ::close(fd);
        return false;
    }
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
        ::close(fd);
        return false;
    }
    const char cmd[] = "SHOW\n";
    if (::write(fd, cmd, sizeof(cmd) - 1) < 0 || !waitSocket(fd, POLLIN, 300)) {
        ::close(fd);
        return false;
    }
    char buf[32];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return std::strncmp(buf, "OK", 2) == 0;
}

// Rounded square the size of the other tray glyphs. The fill is the status:
// green when auto-switch is on, red when it is off, gray when the daemon
// is not listening. The letter is the product mark.
QIcon markIcon(bool on, bool alive) {
    QIcon icon;
    const QColor bg = !alive ? QColor(110, 110, 110)
                              : on ? QColor(46, 125, 50)
                                   : QColor(183, 28, 28);
    for (const int logical : {22, 32}) {
        for (const int scale : {1, 2}) {
            QPixmap pm(logical * scale, logical * scale);
            pm.setDevicePixelRatio(scale);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing, true);
            const qreal S = logical;
            const qreal inset = S * 0.06;
            const QRectF box(inset, inset, S - 2 * inset, S - 2 * inset);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(box, box.width() * 0.22, box.height() * 0.22);
            p.setPen(Qt::white);
            QFont font(QStringLiteral("sans"), 1, QFont::Bold);
            font.setPixelSize(qRound(box.height() * 0.62));
            p.setFont(font);
            p.drawText(box, Qt::AlignCenter, QStringLiteral("Р"));
            icon.addPixmap(pm);
        }
    }
    return icon;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Ракета — настройки"));
    setWindowIcon(markIcon(true, true));
    setMinimumSize(520, 560);
    buildUI();
    loadConfig();
#if PUNTO_WITH_FCITX
    QTimer::singleShot(0, this, &MainWindow::refreshSetupStatus);
#endif
}

MainWindow::~MainWindow() {
    if (instanceNotifier_) instanceNotifier_->setEnabled(false);
    if (instanceFd_ >= 0) {
        ::close(instanceFd_);
        instanceFd_ = -1;
        ::unlink(instanceSocketPath().c_str());
    }
}

bool MainWindow::takeSingleInstance() {
    if (notifyExistingInstance()) return false;

    const std::string path = instanceSocketPath();
    ::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return true;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (path.size() >= sizeof(addr.sun_path)
        || ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
        || ::listen(fd, 4) < 0) {
        ::close(fd);
        if (notifyExistingInstance()) return false;
        return true;
    }
    ::chmod(path.c_str(), 0600);
    instanceFd_ = fd;
    instanceNotifier_ = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(instanceNotifier_, &QSocketNotifier::activated, this, &MainWindow::onInstanceReady);
    return true;
}

void MainWindow::onInstanceReady() {
    if (instanceFd_ < 0) return;
    for (;;) {
        const int client = ::accept4(instanceFd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0) break;
        char buf[32];
        const ssize_t n = ::read(client, buf, sizeof(buf) - 1);
        const char ok[] = "OK\n";
        (void)::write(client, ok, sizeof(ok) - 1);
        ::close(client);
        if (n > 0) {
            show();
            raise();
            activateWindow();
        }
    }
}

#if PUNTO_WITH_FCITX
void MainWindow::buildSetupTab(QTabWidget* tabs) {
    auto* setupTab = new QWidget;
    auto* lay = new QVBoxLayout(setupTab);
    lay->setSpacing(10);
    lay->setContentsMargins(12, 12, 12, 12);

    auto* titleLabel = new QLabel(
        tr("<b>Первый запуск</b><br>"
           "<small>Ракета работает в KDE Wayland через Fcitx5.<br>"
           "Настройка сама перенесёт текущие раскладки.</small>"));
    titleLabel->setWordWrap(true);
    lay->addWidget(titleLabel);

    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    lay->addWidget(sep);

    // Status area
    setupStatusLabel_ = new QLabel(tr("Проверяем состояние…"));
    setupStatusLabel_->setWordWrap(true);
    setupStatusLabel_->setTextFormat(Qt::RichText);
    lay->addWidget(setupStatusLabel_);

    // Buttons
    auto* btnRow = new QHBoxLayout;
    runSetupBtn_ = new QPushButton(tr("Настроить"), this);
    runSetupBtn_->setToolTip(tr("Перенести раскладки KDE в Fcitx5 и выбрать его виртуальной клавиатурой KWin.\n"
                                "Текущие раскладки и сочетание переключения сохраняются."));

    undoSetupBtn_ = new QPushButton(tr("Откатить"), this);
    undoSetupBtn_->setToolTip(tr("Убрать Fcitx5 и вернуть штатный переключатель KDE."));

    auto* refreshBtn = new QPushButton(tr("Обновить"), this);

    btnRow->addWidget(runSetupBtn_);
    btnRow->addWidget(undoSetupBtn_);
    btnRow->addStretch();
    btnRow->addWidget(refreshBtn);
    lay->addLayout(btnRow);

    // Log output for the setup script
    auto* logEdit = new QTextEdit(this);
    logEdit->setReadOnly(true);
    logEdit->setFont(QFont("Monospace", 9));
    logEdit->setPlaceholderText(tr("Здесь будет вывод настройки…"));
    lay->addWidget(logEdit, 1);

    connect(runSetupBtn_, &QPushButton::clicked, this, [this, logEdit]() {
        runSetupBtn_->setEnabled(false);
        logEdit->clear();
        auto* proc = new QProcess(this);
        proc->setProgram("rocket-switcher-setup");
        connect(proc, &QProcess::readyReadStandardOutput, this, [proc, logEdit]() {
            logEdit->append(proc->readAllStandardOutput());
        });
        connect(proc, &QProcess::readyReadStandardError, this, [proc, logEdit]() {
            logEdit->append(QString("<font color='orange'>%1</font>")
                            .arg(QString(proc->readAllStandardError()).toHtmlEscaped()));
        });
        connect(proc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, proc, logEdit](int code, QProcess::ExitStatus) {
            if (code == 0) {
                logEdit->append(tr("<b style='color:green'>Настройка завершена.</b>"));
            } else {
                logEdit->append(tr("<b style='color:red'>Настройка завершилась с кодом %1.</b>").arg(code));
            }
            runSetupBtn_->setEnabled(true);
            proc->deleteLater();
            refreshSetupStatus();
        });
        proc->start();
        if (!proc->waitForStarted(3000)) {
            logEdit->append(tr("<b style='color:red'>Не удалось запустить rocket-switcher-setup. Пакет установлен?</b>"));
            runSetupBtn_->setEnabled(true);
        }
    });

    connect(undoSetupBtn_, &QPushButton::clicked, this, [this, logEdit]() {
        auto ans = QMessageBox::question(this, tr("Откат настройки"),
            tr("Fcitx5 будет убран, вернётся штатная клавиатура KDE.\n"
               "Переключение перестанет работать, пока настройку не запустят снова.\n\nПродолжить?"),
            QMessageBox::Yes | QMessageBox::No);
        if (ans != QMessageBox::Yes) return;
        logEdit->clear();
        auto* proc = new QProcess(this);
        proc->setProgram("rocket-switcher-setup");
        proc->setArguments({"--undo"});
        connect(proc, &QProcess::readyReadStandardOutput, this, [proc, logEdit]() {
            logEdit->append(proc->readAllStandardOutput());
        });
        connect(proc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, proc](int, QProcess::ExitStatus) {
            proc->deleteLater();
            refreshSetupStatus();
        });
        proc->start();
    });

    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshSetupStatus);

    tabs->addTab(setupTab, tr("Настройка"));
}
#endif

void MainWindow::buildUI() {
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(12, 12, 12, 12);

    auto* tabs = new QTabWidget(this);
    mainLayout->addWidget(tabs);

#if PUNTO_WITH_FCITX
    buildSetupTab(tabs);
#endif

#if !PUNTO_WITH_FCITX
    // ---- Tab: Daemon (evdev service) ----
    auto* daemonTab = new QWidget;
    auto* daemonLayout = new QVBoxLayout(daemonTab);
    daemonLayout->setSpacing(8);
    daemonLayout->setContentsMargins(12, 12, 12, 12);

    daemonPassthrough_ = new QCheckBox(tr("Только передавать клавиши, без переключения"), this);
    daemonPassthrough_->setToolTip(
        tr("Выключает слежение за словом, автопереключение и горячие клавиши. "
           "Демон только перехватывает клавиатуру и отдаёт события дальше.\n"
           "Нужно, если клавиши пропадают или залипают. То же, что passthrough=true в daemon.ini."));
    daemonLayout->addWidget(daemonPassthrough_);

    daemonInjectPasteFirst_ = new QCheckBox(
        tr("Wayland: сначала вставка из буфера, потом wtype (KDE / Plasma)"), this);
    daemonInjectPasteFirst_->setToolTip(
        tr("Сначала буфер и Ctrl+V, потом wtype. Включайте на KDE Wayland, если у композитора "
           "нет протокола виртуальной клавиатуры: wtype падает, в журнале "
           "«Compositor does not support…». В графических программах после сна это помогает; "
           "в терминале может понадобиться wtype — снимите флажок, если в Konsole замена ломается."));
    daemonLayout->addWidget(daemonInjectPasteFirst_);

    auto* daemonNote = new QLabel(
        tr("<small><i>Обычный режим оставляет переключение. Прозрачный режим — чтобы проверить, "
           "не мешает ли композитор или uinput. После сохранения служба перезапускается.</i></small>"));
    daemonNote->setWordWrap(true);
    daemonLayout->addWidget(daemonNote);
    daemonLayout->addStretch();

    tabs->addTab(daemonTab, tr("Демон"));
#endif

    // ---- Hotkeys ----
    auto* hotkeysTab = new QWidget;
    auto* hkLayout = new QFormLayout(hotkeysTab);
    hkLayout->setSpacing(8);

    hkSwapLast_      = new HotkeyEditor(this);
    hkSwapSelection_ = new HotkeyEditor(this);
    hkToggleAuto_    = new HotkeyEditor(this);
    hkUndoSwitch_    = new HotkeyEditor(this);

    hkLayout->addRow(tr("Последнее слово:"), hkSwapLast_);
    hkLayout->addRow(tr("Выделенный текст:"), hkSwapSelection_);
    hkLayout->addRow(tr("Автопереключение вкл/выкл:"), hkToggleAuto_);
    hkLayout->addRow(tr("Отменить последнюю замену:"), hkUndoSwitch_);

    auto* hkNote = new QLabel(
        tr("<small><i>Если сочетание уже занято системой или программой, сработает оно. "
           "В журнале будет предупреждение, что клавиша перехвачена без эффекта.</i></small>"));
    hkNote->setWordWrap(true);
    hkLayout->addRow(hkNote);

    tabs->addTab(hotkeysTab, tr("Горячие клавиши"));

    // ---- Tab 2: Auto-Switch ----
    auto* autoTab = new QWidget;
    auto* autoLayout = new QFormLayout(autoTab);
    autoLayout->setSpacing(8);

    autoEnabled_ = new QCheckBox(tr("Автоматически исправлять раскладку"), this);

    minWordLen_ = new QSpinBox(this);
    minWordLen_->setRange(1, 20);
    minWordLen_->setSuffix(tr(" симв."));
    minWordLen_->setToolTip(tr("Слова короче этого порога сами не переключаются "
                               "(чтобы не трогать короткие вроде «я», «в», «a»)."));

    confidence_ = new QDoubleSpinBox(this);
    confidence_->setRange(0.0, 1.0);
    confidence_->setSingleStep(0.05);
    confidence_->setDecimals(2);
    confidence_->setToolTip(
        tr("Насколько другая раскладка должна быть правдоподобнее набранной, "
           "если слова нет в словаре.\n"
           "0,0 — переключать почти всегда, 0,5 — только явные случаи.\n"
           "Обычно хватает 0,10–0,25."));

    autoLayout->addRow(autoEnabled_);
    autoLayout->addRow(tr("Минимальная длина слова:"), minWordLen_);
    autoLayout->addRow(tr("Порог уверенности:"), confidence_);

    neverWords_ = new QPlainTextEdit(this);
    neverWords_->setPlaceholderText(tr("по одному слову в строке"));
    neverWords_->setMinimumHeight(110);
    neverWords_->setToolTip(
        tr("Эти слова не переключаются сами, в любой раскладке.\n"
           "Имена, аббревиатуры, «ok». Строка с # — комментарий."));
    autoLayout->addRow(tr("Не переключать:"), neverWords_);

    auto* autoNote = new QLabel(
        tr("<small><i>Сначала словарь: если набранное уже слово, оно остаётся; "
           "если слово получается в другой раскладке — раскладка меняется. "
           "Для незнакомых слов сравниваются биграммы. Надёжнее всего от 4 букв. "
           "Числа, адреса и почта не трогаются.</i></small>"));
    autoNote->setWordWrap(true);
    autoLayout->addRow(autoNote);

    tabs->addTab(autoTab, tr("Автопереключение"));

    // ---- Tab: typos ----
    auto* typoTab = new QWidget;
    auto* typoLayout = new QFormLayout(typoTab);
    typoLayout->setSpacing(8);

    typoEnabled_ = new QCheckBox(tr("Исправлять опечатки"), this);
    typoEnabled_->setToolTip(
        tr("Отдельно от автопереключения. Срабатывает, когда слово решили не переводить."));

    typoRules_ = new QPlainTextEdit(this);
    typoRules_->setPlaceholderText(tr("почемуто = почему-то"));
    typoRules_->setMinimumHeight(160);
    typoRules_->setToolTip(
        tr("Точные замены, по одной в строке: как набрали = как должно быть.\n"
           "Можно писать «->». Строка с # — комментарий.\n"
           "Пустой файл отключает только эти замены."));
    typoLayout->addRow(typoEnabled_);
    typoLayout->addRow(tr("Правила:"), typoRules_);

    auto* typoNote = new QLabel(
        tr("<small><i>Кроме списка, при включённом переключателе правятся: "
           "две заглавные в начале («ПРивет»), буква с клавиши знака "
           "(«пирогб» → «пирог,», «привет1» → «привет!», «9хорошо» → «(хорошо»), "
           "пробел в «приветЭто», если обе части есть в словаре, "
           "и одна опечатка, если в словаре ровно один вариант "
           "(«привте» → «привет»). Два варианта не трогаются: «bokk» похоже "
           "и на «book», и на «boko».</i></small>"));
    typoNote->setWordWrap(true);
    typoLayout->addRow(typoNote);

    tabs->addTab(typoTab, tr("Опечатки"));

    // ---- Tab 3: Debug / Log ----
    auto* debugTab = new QWidget;
    auto* dbgLayout = new QFormLayout(debugTab);
    dbgLayout->setSpacing(8);

#if PUNTO_WITH_FCITX
    logLevel_ = new QComboBox(this);
    logLevel_->addItem(tr("Выключен"), QStringLiteral("Disabled"));
    logLevel_->addItem(tr("Предупреждения"), QStringLiteral("Warning"));
    logLevel_->addItem(tr("Отладка"), QStringLiteral("Debug"));
    logLevel_->addItem(tr("Подробно"), QStringLiteral("Verbose"));
    dbgLayout->addRow(tr("Журнал Fcitx5:"), logLevel_);

    auto* dbgNote = new QLabel(
        tr("<small>Журнал пишется в journald и ~/.local/share/fcitx5/logs/.<br>"
           "После смены уровня Fcitx5 перезагружается.</small>"));
#else
    auto* dbgNote = new QLabel(
        tr("<small>Если демон запущен из терминала, диагностика идёт в stderr.<br>"
           "После сохранения делается попытка перезапуска: <code>systemctl --user restart "
           "rocket-switcher-daemon</code>.</small>"));
#endif
    dbgNote->setWordWrap(true);
    dbgLayout->addRow(dbgNote);

    tabs->addTab(debugTab, tr("Отладка"));

    // ---- Bottom buttons ----
    auto* btnLayout = new QHBoxLayout;
    defaultsBtn_ = new QPushButton(tr("По умолчанию"), this);
    saveBtn_     = new QPushButton(tr("Сохранить"), this);
    cancelBtn_   = new QPushButton(tr("Закрыть"), this);

    saveBtn_->setDefault(true);
    saveBtn_->setIcon(QApplication::style()->standardIcon(QStyle::SP_DialogSaveButton));
    cancelBtn_->setIcon(QApplication::style()->standardIcon(QStyle::SP_DialogCloseButton));

    btnLayout->addWidget(defaultsBtn_);
    btnLayout->addStretch();
    btnLayout->addWidget(cancelBtn_);
    btnLayout->addWidget(saveBtn_);
    mainLayout->addLayout(btnLayout);

    // Status bar
    statusBar()->showMessage(tr("Готово"));

    connect(saveBtn_,     &QPushButton::clicked, this, &MainWindow::onSave);
    connect(cancelBtn_,   &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(defaultsBtn_, &QPushButton::clicked, this, &MainWindow::onRestoreDefaults);
}

QString MainWindow::configPath() const {
    QString home = QDir::homePath();
#if PUNTO_WITH_FCITX
    return home + "/.config/fcitx5/conf/punto-switcher.conf";
#else
    return home + "/.config/rocket-switcher/daemon.ini";
#endif
}

#if !PUNTO_WITH_FCITX
static QString configPathToLoad() {
    const QString home = QDir::homePath();
    const QString neu = home + "/.config/rocket-switcher/daemon.ini";
    const QString old = home + "/.config/punto-switcher/daemon.ini";
    if (QFileInfo::exists(neu) || !QFileInfo::exists(old)) return neu;
    return old;
}
#endif

void MainWindow::loadConfig() {
#if PUNTO_WITH_FCITX
    QSettings cfg(configPath(), QSettings::IniFormat);
#else
    QSettings cfg(configPathToLoad(), QSettings::IniFormat);
#endif

#if PUNTO_WITH_FCITX
    hkSwapLast_->setFromFcitxString(
        cfg.value("SwapLastText", DEFAULT_SWAP_LAST).toString());
    hkSwapSelection_->setFromFcitxString(
        cfg.value("SwapSelection", DEFAULT_SWAP_SELECTION).toString());
    hkToggleAuto_->setFromFcitxString(
        cfg.value("ToggleAutoSwitch", DEFAULT_TOGGLE_AUTO).toString());
    hkUndoSwitch_->setFromFcitxString(
        cfg.value("UndoLastSwitch", DEFAULT_UNDO_SWITCH).toString());

    autoEnabled_->setChecked(
        cfg.value("AutoSwitch", true).toBool());
    if (typoEnabled_)
        typoEnabled_->setChecked(cfg.value("TypoCorrect", true).toBool());
    minWordLen_->setValue(
        cfg.value("MinWordLength", DEFAULT_MIN_WORD_LEN).toInt());
    int pct = cfg.value("ConfidenceThresholdPct",
                        qRound(DEFAULT_CONFIDENCE * 100)).toInt();
    confidence_->setValue(pct / 100.0);

    QString ll = cfg.value("LogLevel", "Warning").toString();
    int idx = logLevel_->findData(ll);
    if (idx < 0) idx = logLevel_->findText(ll);
    logLevel_->setCurrentIndex(idx >= 0 ? idx : 1);
#else
    cfg.beginGroup("hotkeys");
    hkSwapLast_->setFromFcitxString(
        cfg.value("swap_last", DEFAULT_SWAP_LAST).toString());
    hkSwapSelection_->setFromFcitxString(
        cfg.value("swap_selection", DEFAULT_SWAP_SELECTION).toString());
    hkToggleAuto_->setFromFcitxString(
        cfg.value("toggle_auto", DEFAULT_TOGGLE_AUTO).toString());
    hkUndoSwitch_->setFromFcitxString(
        cfg.value("undo", DEFAULT_UNDO_SWITCH).toString());
    cfg.endGroup();

    cfg.beginGroup("daemon");
    if (daemonPassthrough_) {
        daemonPassthrough_->setChecked(cfg.value("passthrough", false).toBool());
    }
    if (daemonInjectPasteFirst_) {
        daemonInjectPasteFirst_->setChecked(cfg.value("inject_paste_first", false).toBool());
    }
    cfg.endGroup();

    cfg.beginGroup("autoswitch");
    autoEnabled_->setChecked(cfg.value("enabled", true).toBool());
    minWordLen_->setValue(cfg.value("min_word_length", DEFAULT_MIN_WORD_LEN).toInt());
    confidence_->setValue(cfg.value("confidence_threshold", DEFAULT_CONFIDENCE).toDouble());
    cfg.endGroup();
    typoEnabled_->setChecked(cfg.value("autoswitch/typo_correct", true).toBool());
#endif
    loadNeverWords();
    loadTypoRules();
}

void MainWindow::saveConfig() {
    QDir().mkpath(QFileInfo(configPath()).absolutePath());

    QSettings cfg(configPath(), QSettings::IniFormat);

#if PUNTO_WITH_FCITX
    cfg.setValue("SwapLastText",       hkSwapLast_->fcitxString());
    cfg.setValue("SwapSelection",      hkSwapSelection_->fcitxString());
    cfg.setValue("ToggleAutoSwitch",   hkToggleAuto_->fcitxString());
    cfg.setValue("UndoLastSwitch",    hkUndoSwitch_->fcitxString());
    cfg.setValue("AutoSwitch",            autoEnabled_->isChecked());
    if (typoEnabled_)
        cfg.setValue("TypoCorrect", typoEnabled_->isChecked());
    cfg.setValue("MinWordLength",         minWordLen_->value());
    cfg.setValue("ConfidenceThresholdPct", qRound(confidence_->value() * 100));
    cfg.setValue("LogLevel",           logLevel_->currentData().toString());
#else
    cfg.beginGroup("hotkeys");
    cfg.setValue("swap_last",      hkSwapLast_->fcitxString());
    cfg.setValue("swap_selection", hkSwapSelection_->fcitxString());
    cfg.setValue("toggle_auto",    hkToggleAuto_->fcitxString());
    cfg.setValue("undo",           hkUndoSwitch_->fcitxString());
    cfg.endGroup();
    cfg.beginGroup("daemon");
    if (daemonPassthrough_) {
        cfg.setValue("passthrough", daemonPassthrough_->isChecked());
    }
    if (daemonInjectPasteFirst_) {
        cfg.setValue("inject_paste_first", daemonInjectPasteFirst_->isChecked());
    }
    cfg.endGroup();
    cfg.beginGroup("autoswitch");
    cfg.setValue("enabled", autoEnabled_->isChecked());
    cfg.setValue("min_word_length", minWordLen_->value());
    cfg.setValue("confidence_threshold", confidence_->value());
    cfg.setValue("typo_correct", typoEnabled_ && typoEnabled_->isChecked());
    cfg.endGroup();
#endif

    cfg.sync();
    saveNeverWords();
    saveTypoRules();
}

#if PUNTO_WITH_FCITX
void MainWindow::reloadFcitx5() {
    QProcess::startDetached("fcitx5-remote", {"-r"});
}
#endif

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MainWindow::onSave() {
    saveConfig();
#if PUNTO_WITH_FCITX
    reloadFcitx5();
    statusBar()->showMessage(tr("Настройки сохранены, Fcitx5 перезагружается."), 3000);
#else
    const QString reloaded = daemonCommand(QStringLiteral("RELOAD"));
    QProcess::startDetached("systemctl", {"--user", "try-restart", "rocket-switcher-daemon"});
    statusBar()->showMessage(
        reloaded == QLatin1String("OK")
            ? tr("Настройки сохранены. Список исключений уже у демона, служба перезапускается.")
            : tr("Настройки сохранены, служба перезапускается."),
        4000);
#endif
}

void MainWindow::onCancel() {
    close();
}

QString MainWindow::neverWordsPath() const {
    return QDir::homePath() + "/.config/rocket-switcher/dict/never.txt";
}

void MainWindow::loadNeverWords() {
    if (!neverWords_) return;
    QString path = neverWordsPath();
    const QString old = QDir::homePath() + "/.config/punto-switcher/dict/never.txt";
    if (!QFileInfo::exists(path) && QFileInfo::exists(old)) path = old;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        neverWords_->clear();
        return;
    }
    neverWords_->setPlainText(QString::fromUtf8(file.readAll()));
}

void MainWindow::saveNeverWords() {
    if (!neverWords_) return;
    const QString path = neverWordsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    QByteArray data = neverWords_->toPlainText().toUtf8();
    if (!data.isEmpty() && !data.endsWith('\n')) data.append('\n');
    file.write(data);
}

QString MainWindow::typoRulesPath() const {
    return QDir::homePath() + "/.config/rocket-switcher/dict/typos.txt";
}

void MainWindow::loadTypoRules() {
    if (!typoRules_) return;
    QString path = typoRulesPath();
    const QString old = QDir::homePath() + "/.config/punto-switcher/dict/typos.txt";
    if (!QFileInfo::exists(path) && QFileInfo::exists(old)) path = old;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        typoRules_->setPlainText(QString::fromStdString(punto::TypoCorrector::defaultRulesText()));
        return;
    }
    typoRules_->setPlainText(QString::fromUtf8(file.readAll()));
}

void MainWindow::saveTypoRules() {
    if (!typoRules_) return;
    const QString path = typoRulesPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    QByteArray data = typoRules_->toPlainText().toUtf8();
    if (!data.isEmpty() && !data.endsWith('\n')) data.append('\n');
    file.write(data);
}

void MainWindow::setupTray() {
#if PUNTO_WITH_FCITX
    return;
#else
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    QApplication::setQuitOnLastWindowClosed(false);

    trayIcon_ = new QSystemTrayIcon(markIcon(true, false), this);
    auto* menu = new QMenu(this);
    trayAuto_ = menu->addAction(tr("Автопереключение"));
    trayAuto_->setCheckable(true);
    menu->addSeparator();
    auto* showAct = menu->addAction(tr("Настройки"));
    auto* quitAct = menu->addAction(tr("Выход"));
    quitAct->setToolTip(tr("Закрыть значок. Демон продолжит работать."));
    trayIcon_->setContextMenu(menu);

    connect(trayAuto_, &QAction::toggled, this, [this](bool on) {
        const QString reply = daemonCommand(on ? QStringLiteral("AUTO 1") : QStringLiteral("AUTO 0"));
        if (reply != QLatin1String("OK")) {
            refreshTray();
            return;
        }
        trayIcon_->setIcon(markIcon(on, true));
        trayIcon_->setToolTip(on ? tr("Ракета: автопереключение включено")
                                 : tr("Ракета: автопереключение выключено"));
        if (autoEnabled_) autoEnabled_->setChecked(on);
    });
    connect(showAct, &QAction::triggered, this, [this]() {
        show();
        raise();
        activateWindow();
    });
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);
    connect(trayIcon_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason != QSystemTrayIcon::Trigger && reason != QSystemTrayIcon::DoubleClick)
                    return;
                if (isVisible()) hide();
                else {
                    show();
                    raise();
                    activateWindow();
                }
            });

    auto* timer = new QTimer(this);
    timer->setInterval(1500);
    connect(timer, &QTimer::timeout, this, &MainWindow::refreshTray);
    timer->start();
    refreshTray();
    trayIcon_->show();
#endif
}

void MainWindow::refreshTray() {
    if (!trayIcon_) return;
    const QString reply = daemonCommand(QStringLiteral("STATUS"));
    const bool alive = reply == QLatin1String("AUTO 1") || reply == QLatin1String("AUTO 0");
    const bool on = reply == QLatin1String("AUTO 1");
    if (trayAuto_) {
        const QSignalBlocker block(trayAuto_);
        trayAuto_->setChecked(on);
        trayAuto_->setEnabled(alive);
    }
    trayIcon_->setIcon(markIcon(on, alive));
    if (!alive)
        trayIcon_->setToolTip(tr("Ракета: демон не запущен"));
    else if (on)
        trayIcon_->setToolTip(tr("Ракета: автопереключение включено"));
    else
        trayIcon_->setToolTip(tr("Ракета: автопереключение выключено"));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (trayIcon_ && trayIcon_->isVisible()) {
        hide();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
#if !PUNTO_WITH_FCITX
    const QString reply = daemonCommand(QStringLiteral("STATUS"));
    if (autoEnabled_ && (reply == QLatin1String("AUTO 1") || reply == QLatin1String("AUTO 0")))
        autoEnabled_->setChecked(reply == QLatin1String("AUTO 1"));
#endif
}

void MainWindow::onRestoreDefaults() {
    auto answer = QMessageBox::question(
        this, tr("Настройки по умолчанию"),
        tr("Вернуть все настройки к исходным?"),
        QMessageBox::Yes | QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    hkSwapLast_->setFromFcitxString(DEFAULT_SWAP_LAST);
    hkSwapSelection_->setFromFcitxString(DEFAULT_SWAP_SELECTION);
    hkToggleAuto_->setFromFcitxString(DEFAULT_TOGGLE_AUTO);
    hkUndoSwitch_->setFromFcitxString(DEFAULT_UNDO_SWITCH);
    autoEnabled_->setChecked(true);
    minWordLen_->setValue(DEFAULT_MIN_WORD_LEN);
    confidence_->setValue(DEFAULT_CONFIDENCE);
    if (neverWords_) neverWords_->clear();
    if (typoEnabled_) typoEnabled_->setChecked(true);
    if (typoRules_)
        typoRules_->setPlainText(QString::fromStdString(punto::TypoCorrector::defaultRulesText()));
#if !PUNTO_WITH_FCITX
    if (daemonPassthrough_) daemonPassthrough_->setChecked(false);
    if (daemonInjectPasteFirst_) daemonInjectPasteFirst_->setChecked(false);
#endif
#if PUNTO_WITH_FCITX
    logLevel_->setCurrentIndex(1);
#endif

    statusBar()->showMessage(tr("Подставлены значения по умолчанию. Нажмите «Сохранить»."), 3000);
}

void MainWindow::onAbout() {
    QMessageBox::about(
        this,
        tr("О программе"),
        tr("<b>Ракета</b> v1.0<br>"
#if PUNTO_WITH_FCITX
           "Автоисправление раскладки ru↔en (модуль Fcitx5).<br><br>"
#else
           "Автоисправление раскладки ru↔en (демон evdev, без Fcitx5).<br><br>"
#endif
           "Лицензия: GPL-2.0+<br>"
           "Исходный код: https://github.com/your-org/punto-switcher"));
}

void MainWindow::refreshSetupStatus() {
#if !PUNTO_WITH_FCITX
    return;
#else
    // Run 'rocket-switcher-setup --status' and parse output
    auto* proc = new QProcess(this);
    proc->setProgram("rocket-switcher-setup");
    proc->setArguments({"--status"});
    connect(proc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int, QProcess::ExitStatus) {
        QString out = proc->readAllStandardOutput();
        proc->deleteLater();

        bool hasProfile  = out.contains("Fcitx5 profile:") && out.contains("present");
        bool hasVK       = out.contains("KWin virtual keyboard:") && out.contains("Fcitx5 ✓");
        bool hasPlugin   = out.contains("Fcitx5 KWin plugin:") && out.contains("found");
        bool isRunning   = out.contains("Fcitx5 running:") && out.contains("yes");

        QString html;
        auto row = [&](bool ok, const QString& label, const QString& fix = {}) {
            QString icon = ok ? "✅" : "❌";
            QString color = ok ? "green" : "red";
            html += QString("<p>%1 <span style='color:%2'>%3</span>").arg(icon, color, label);
            if (!ok && !fix.isEmpty())
                html += QString(" — <small><i>%1</i></small>").arg(fix);
            html += "</p>";
        };

        bool allOk = hasProfile && hasVK && hasPlugin && isRunning;

        if (allOk) {
            html = "<p><b style='color:green'>✅ Ракета настроена и работает.</b></p>";
        } else {
            html = "<p><b style='color:orange'>Нужна настройка. Нажмите «Настроить».</b></p>";
            row(hasPlugin,  tr("Плагин Fcitx5 для KWin установлен"),
                tr("sudo apt install kde-config-fcitx5"));
            row(hasProfile, tr("Раскладки перенесены в Fcitx5"),
                tr("Нажмите «Настроить»"));
            row(hasVK,      tr("Fcitx5 выбран виртуальной клавиатурой KWin"),
                tr("Нажмите «Настроить» или выберите вручную: Параметры системы → Виртуальная клавиатура"));
            row(isRunning,  tr("Fcitx5 запущен"),
                tr("Нажмите «Настроить», чтобы запустить"));
        }
        setupStatusLabel_->setText(html);
        runSetupBtn_->setEnabled(!allOk || true); // always allow re-running
    });
    proc->start();
    if (!proc->waitForStarted(3000)) {
        setupStatusLabel_->setText(
            tr("<p><b style='color:red'>❌ rocket-switcher-setup не найден.</b> "
               "Переустановите пакет.</p>"));
    }
#endif
}

void MainWindow::onRunSetup() {}
void MainWindow::onUndoSetup() {}
