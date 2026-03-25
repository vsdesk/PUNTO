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
#include <QTimer>

// Default hotkey strings (Fcitx5 format)
static const char* DEFAULT_SWAP_LAST      = "Alt+apostrophe";
static const char* DEFAULT_SWAP_SELECTION = "Alt+shift+apostrophe";
static const char* DEFAULT_TOGGLE_AUTO    = "Alt+shift+a";
static const int    DEFAULT_MIN_WORD_LEN  = 3;
static const double DEFAULT_CONFIDENCE    = 0.15;

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Punto Switcher — Settings"));
    setMinimumSize(520, 480);
    buildUI();
    loadConfig();
    // Refresh setup status after the window is shown
    QTimer::singleShot(0, this, &MainWindow::refreshSetupStatus);
}

void MainWindow::buildSetupTab(QTabWidget* tabs) {
    auto* setupTab = new QWidget;
    auto* lay = new QVBoxLayout(setupTab);
    lay->setSpacing(10);
    lay->setContentsMargins(12, 12, 12, 12);

    auto* titleLabel = new QLabel(
        tr("<b>First-time setup</b><br>"
           "<small>Punto Switcher requires Fcitx5 as the active input method in KDE Wayland.<br>"
           "The setup will migrate your existing keyboard layouts automatically.</small>"));
    titleLabel->setWordWrap(true);
    lay->addWidget(titleLabel);

    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    lay->addWidget(sep);

    // Status area
    setupStatusLabel_ = new QLabel(tr("Checking setup status…"));
    setupStatusLabel_->setWordWrap(true);
    setupStatusLabel_->setTextFormat(Qt::RichText);
    lay->addWidget(setupStatusLabel_);

    // Buttons
    auto* btnRow = new QHBoxLayout;
    runSetupBtn_ = new QPushButton(tr("Run Setup"), this);
    runSetupBtn_->setToolTip(tr("Migrate your KDE keyboard layouts to Fcitx5 and configure KWin virtual keyboard.\n"
                                "Your existing layouts and switching shortcut are preserved."));

    undoSetupBtn_ = new QPushButton(tr("Undo Setup"), this);
    undoSetupBtn_->setToolTip(tr("Remove Fcitx5 as virtual keyboard and restore KDE native keyboard switcher."));

    auto* refreshBtn = new QPushButton(tr("Refresh Status"), this);

    btnRow->addWidget(runSetupBtn_);
    btnRow->addWidget(undoSetupBtn_);
    btnRow->addStretch();
    btnRow->addWidget(refreshBtn);
    lay->addLayout(btnRow);

    // Log output for the setup script
    auto* logEdit = new QTextEdit(this);
    logEdit->setReadOnly(true);
    logEdit->setFont(QFont("Monospace", 9));
    logEdit->setPlaceholderText(tr("Setup output will appear here…"));
    lay->addWidget(logEdit, 1);

    connect(runSetupBtn_, &QPushButton::clicked, this, [this, logEdit]() {
        runSetupBtn_->setEnabled(false);
        logEdit->clear();
        auto* proc = new QProcess(this);
        proc->setProgram("punto-switcher-setup");
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
                logEdit->append(tr("<b style='color:green'>Setup completed successfully.</b>"));
            } else {
                logEdit->append(tr("<b style='color:red'>Setup exited with code %1.</b>").arg(code));
            }
            runSetupBtn_->setEnabled(true);
            proc->deleteLater();
            refreshSetupStatus();
        });
        proc->start();
        if (!proc->waitForStarted(3000)) {
            logEdit->append(tr("<b style='color:red'>Could not start punto-switcher-setup. Is the package installed correctly?</b>"));
            runSetupBtn_->setEnabled(true);
        }
    });

    connect(undoSetupBtn_, &QPushButton::clicked, this, [this, logEdit]() {
        auto ans = QMessageBox::question(this, tr("Undo Setup"),
            tr("This will remove Fcitx5 as the virtual keyboard and restore KDE's native keyboard.\n"
               "Punto Switcher will stop working until you run Setup again.\n\nContinue?"),
            QMessageBox::Yes | QMessageBox::No);
        if (ans != QMessageBox::Yes) return;
        logEdit->clear();
        auto* proc = new QProcess(this);
        proc->setProgram("punto-switcher-setup");
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

    tabs->addTab(setupTab, tr("Setup"));
}

void MainWindow::buildUI() {
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(12, 12, 12, 12);

    auto* tabs = new QTabWidget(this);
    mainLayout->addWidget(tabs);

    // ---- Tab 0: Setup ----
    buildSetupTab(tabs);

    // ---- Tab 1: Hotkeys ----
    auto* hotkeysTab = new QWidget;
    auto* hkLayout = new QFormLayout(hotkeysTab);
    hkLayout->setSpacing(8);

    hkSwapLast_      = new HotkeyEditor(this);
    hkSwapSelection_ = new HotkeyEditor(this);
    hkToggleAuto_    = new HotkeyEditor(this);

    hkLayout->addRow(tr("Swap last word:"), hkSwapLast_);
    hkLayout->addRow(tr("Swap selection:"), hkSwapSelection_);
    hkLayout->addRow(tr("Toggle auto-switch:"), hkToggleAuto_);

    auto* hkNote = new QLabel(
        tr("<small><i>Conflict note: if a hotkey conflicts with system or application "
           "shortcuts, the system binding takes priority. Punto will log a warning "
           "when the key is intercepted without effect.</i></small>"));
    hkNote->setWordWrap(true);
    hkLayout->addRow(hkNote);

    tabs->addTab(hotkeysTab, tr("Hotkeys"));

    // ---- Tab 2: Auto-Switch ----
    auto* autoTab = new QWidget;
    auto* autoLayout = new QFormLayout(autoTab);
    autoLayout->setSpacing(8);

    autoEnabled_ = new QCheckBox(tr("Enable automatic layout switching"), this);

    minWordLen_ = new QSpinBox(this);
    minWordLen_->setRange(1, 20);
    minWordLen_->setSuffix(tr(" chars"));
    minWordLen_->setToolTip(tr("Words shorter than this are never auto-switched "
                               "(prevents false positives on short words like 'a', 'I', 'в')."));

    confidence_ = new QDoubleSpinBox(this);
    confidence_->setRange(0.0, 1.0);
    confidence_->setSingleStep(0.05);
    confidence_->setDecimals(2);
    confidence_->setToolTip(
        tr("How much better the swapped version must score in bigram frequency "
           "analysis compared to the original.\n"
           "0.0 = switch almost always  |  0.5 = only very obvious cases.\n"
           "Recommended: 0.10 – 0.25"));

    autoLayout->addRow(autoEnabled_);
    autoLayout->addRow(tr("Minimum word length:"), minWordLen_);
    autoLayout->addRow(tr("Confidence threshold:"), confidence_);

    auto* autoNote = new QLabel(
        tr("<small><i>Auto-switch uses bigram frequency scoring (no dictionary, no ML). "
           "It fires when a word, mapped to the other layout, scores better than "
           "the original. Works best for words ≥ 4 characters. "
           "Numbers, URLs, and emails are always excluded.</i></small>"));
    autoNote->setWordWrap(true);
    autoLayout->addRow(autoNote);

    tabs->addTab(autoTab, tr("Auto-Switch"));

    // ---- Tab 3: Debug / Log ----
    auto* debugTab = new QWidget;
    auto* dbgLayout = new QFormLayout(debugTab);
    dbgLayout->setSpacing(8);

    logLevel_ = new QComboBox(this);
    logLevel_->addItems({tr("Disabled"), tr("Warning"), tr("Debug"), tr("Verbose")});
    dbgLayout->addRow(tr("Fcitx5 log level:"), logLevel_);

    auto* dbgNote = new QLabel(
        tr("<small>Log output goes to journald / ~/.local/share/fcitx5/logs/.<br>"
           "Reload Fcitx5 after changing log level.</small>"));
    dbgNote->setWordWrap(true);
    dbgLayout->addRow(dbgNote);

    tabs->addTab(debugTab, tr("Debug"));

    // ---- Bottom buttons ----
    auto* btnLayout = new QHBoxLayout;
    defaultsBtn_ = new QPushButton(tr("Restore Defaults"), this);
    saveBtn_     = new QPushButton(tr("Save && Reload"), this);
    cancelBtn_   = new QPushButton(tr("Close"), this);

    saveBtn_->setDefault(true);
    saveBtn_->setIcon(QApplication::style()->standardIcon(QStyle::SP_DialogSaveButton));
    cancelBtn_->setIcon(QApplication::style()->standardIcon(QStyle::SP_DialogCloseButton));

    btnLayout->addWidget(defaultsBtn_);
    btnLayout->addStretch();
    btnLayout->addWidget(cancelBtn_);
    btnLayout->addWidget(saveBtn_);
    mainLayout->addLayout(btnLayout);

    // Status bar
    statusBar()->showMessage(tr("Ready"));

    connect(saveBtn_,     &QPushButton::clicked, this, &MainWindow::onSave);
    connect(cancelBtn_,   &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(defaultsBtn_, &QPushButton::clicked, this, &MainWindow::onRestoreDefaults);
}

// ---------------------------------------------------------------------------
// Config path: ~/.config/fcitx5/conf/punto-switcher.conf
// ---------------------------------------------------------------------------

QString MainWindow::configPath() const {
    QString home = QDir::homePath();
    return home + "/.config/fcitx5/conf/punto-switcher.conf";
}

void MainWindow::loadConfig() {
    QSettings cfg(configPath(), QSettings::IniFormat);

    // QSettings with INI format: keys are plain strings, groups = sections
    hkSwapLast_->setFromFcitxString(
        cfg.value("SwapLastText", DEFAULT_SWAP_LAST).toString());
    hkSwapSelection_->setFromFcitxString(
        cfg.value("SwapSelection", DEFAULT_SWAP_SELECTION).toString());
    hkToggleAuto_->setFromFcitxString(
        cfg.value("ToggleAutoSwitch", DEFAULT_TOGGLE_AUTO).toString());

    autoEnabled_->setChecked(
        cfg.value("AutoSwitch", true).toBool());
    minWordLen_->setValue(
        cfg.value("MinWordLength", DEFAULT_MIN_WORD_LEN).toInt());
    // ConfidenceThresholdPct stores threshold * 100 as integer (e.g. 15 = 0.15)
    int pct = cfg.value("ConfidenceThresholdPct",
                        qRound(DEFAULT_CONFIDENCE * 100)).toInt();
    confidence_->setValue(pct / 100.0);

    QString ll = cfg.value("LogLevel", "Warning").toString();
    int idx = logLevel_->findText(ll);
    logLevel_->setCurrentIndex(idx >= 0 ? idx : 1);
}

void MainWindow::saveConfig() {
    // Ensure directory exists
    QDir().mkpath(QFileInfo(configPath()).absolutePath());

    QSettings cfg(configPath(), QSettings::IniFormat);

    cfg.setValue("SwapLastText",       hkSwapLast_->fcitxString());
    cfg.setValue("SwapSelection",      hkSwapSelection_->fcitxString());
    cfg.setValue("ToggleAutoSwitch",   hkToggleAuto_->fcitxString());
    cfg.setValue("AutoSwitch",            autoEnabled_->isChecked());
    cfg.setValue("MinWordLength",         minWordLen_->value());
    cfg.setValue("ConfidenceThresholdPct", qRound(confidence_->value() * 100));
    cfg.setValue("LogLevel",           logLevel_->currentText());

    cfg.sync();
}

void MainWindow::reloadFcitx5() {
    // Send D-Bus reload signal to Fcitx5, ignoring errors if it's not running
    QProcess::startDetached("fcitx5-remote", {"-r"});
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MainWindow::onSave() {
    saveConfig();
    reloadFcitx5();
    statusBar()->showMessage(tr("Settings saved. Fcitx5 reload requested."), 3000);
}

void MainWindow::onCancel() {
    close();
}

void MainWindow::onRestoreDefaults() {
    auto answer = QMessageBox::question(
        this, tr("Restore Defaults"),
        tr("Reset all settings to defaults?"),
        QMessageBox::Yes | QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    hkSwapLast_->setFromFcitxString(DEFAULT_SWAP_LAST);
    hkSwapSelection_->setFromFcitxString(DEFAULT_SWAP_SELECTION);
    hkToggleAuto_->setFromFcitxString(DEFAULT_TOGGLE_AUTO);
    autoEnabled_->setChecked(true);
    minWordLen_->setValue(DEFAULT_MIN_WORD_LEN);
    confidence_->setValue(DEFAULT_CONFIDENCE);
    logLevel_->setCurrentIndex(1);

    statusBar()->showMessage(tr("Defaults restored (not saved yet — click Save)"), 3000);
}

void MainWindow::onAbout() {
    QMessageBox::about(
        this,
        tr("About Punto Switcher"),
        tr("<b>Punto Switcher</b> v1.0<br>"
           "Automatic ru↔en keyboard layout corrector for Wayland/Fcitx5.<br><br>"
           "License: GPL-2.0+<br>"
           "Source: https://github.com/your-org/punto-switcher"));
}

void MainWindow::refreshSetupStatus() {
    // Run 'punto-switcher-setup --status' and parse output
    auto* proc = new QProcess(this);
    proc->setProgram("punto-switcher-setup");
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
            html = "<p><b style='color:green'>✅ Punto Switcher is fully configured and active.</b></p>";
        } else {
            html = "<p><b style='color:orange'>Setup required. Click \"Run Setup\" to fix automatically.</b></p>";
            row(hasPlugin,  tr("Fcitx5 KWin plugin installed"),
                tr("sudo apt install kde-config-fcitx5"));
            row(hasProfile, tr("Keyboard layouts migrated to Fcitx5"),
                tr("Click \"Run Setup\""));
            row(hasVK,      tr("Fcitx5 set as KWin virtual keyboard"),
                tr("Click \"Run Setup\" or set manually in System Settings → Virtual Keyboard"));
            row(isRunning,  tr("Fcitx5 is running"),
                tr("Click \"Run Setup\" to start it"));
        }
        setupStatusLabel_->setText(html);
        runSetupBtn_->setEnabled(!allOk || true); // always allow re-running
    });
    proc->start();
    if (!proc->waitForStarted(3000)) {
        setupStatusLabel_->setText(
            tr("<p><b style='color:red'>❌ punto-switcher-setup not found.</b> "
               "Try reinstalling the package.</p>"));
    }
}

void MainWindow::onRunSetup() {}
void MainWindow::onUndoSetup() {}
