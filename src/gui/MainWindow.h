#pragma once
#include <QMainWindow>
#include <QSettings>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QComboBox>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QSystemTrayIcon>
#include "HotkeyEditor.h"

class QAction;
class QSocketNotifier;
class QCloseEvent;
class QShowEvent;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // False when another settings window is already running: that window was asked to show itself.
    bool takeSingleInstance();
    void setupTray();

private slots:
    void onSave();
    void onCancel();
    void onRestoreDefaults();
    void onAbout();
    void onRunSetup();
    void onUndoSetup();
    void refreshSetupStatus();

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUI();
#if PUNTO_WITH_FCITX
    void buildSetupTab(QTabWidget* tabs);
#endif
    void loadConfig();
    void saveConfig();
    void loadNeverWords();
    void saveNeverWords();
    QString neverWordsPath() const;
    void loadTypoRules();
    void saveTypoRules();
    QString typoRulesPath() const;
    void refreshTray();
    QString configPath() const;
    void reloadFcitx5();
    void onInstanceReady();

    // Setup tab
    QLabel*      setupStatusLabel_;
    QPushButton* runSetupBtn_;
    QPushButton* undoSetupBtn_;

    // Hotkeys tab
    HotkeyEditor* hkSwapLast_;
    HotkeyEditor* hkSwapSelection_;
    HotkeyEditor* hkToggleAuto_;
    HotkeyEditor* hkUndoSwitch_;

    // Auto-switch tab
    QCheckBox*      autoEnabled_;
    QCheckBox*      soundEnabled_ = nullptr;
    QSpinBox*       minWordLen_;
    QDoubleSpinBox* confidence_;
    QPlainTextEdit* neverWords_ = nullptr;
    QCheckBox*      typoEnabled_ = nullptr;
    QPlainTextEdit* typoRules_ = nullptr;

    QSystemTrayIcon* trayIcon_ = nullptr;
    QAction*         trayAuto_ = nullptr;
    int              instanceFd_ = -1;
    QSocketNotifier* instanceNotifier_ = nullptr;

    // Debug tab (Fcitx5 build only)
    QComboBox* logLevel_ = nullptr;

    // Daemon-only: ~/.config/rocket-switcher/daemon.ini [daemon]
    QCheckBox* daemonPassthrough_     = nullptr;
    QCheckBox* daemonInjectPasteFirst_ = nullptr;

    QPushButton* saveBtn_;
    QPushButton* cancelBtn_;
    QPushButton* defaultsBtn_;
};
