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
#include "HotkeyEditor.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onSave();
    void onCancel();
    void onRestoreDefaults();
    void onAbout();
    void onRunSetup();
    void onUndoSetup();
    void refreshSetupStatus();

private:
    void buildUI();
    void buildSetupTab(QTabWidget* tabs);
    void loadConfig();
    void saveConfig();
    QString configPath() const;
    void reloadFcitx5();

    // Setup tab
    QLabel*      setupStatusLabel_;
    QPushButton* runSetupBtn_;
    QPushButton* undoSetupBtn_;

    // Hotkeys tab
    HotkeyEditor* hkSwapLast_;
    HotkeyEditor* hkSwapSelection_;
    HotkeyEditor* hkToggleAuto_;

    // Auto-switch tab
    QCheckBox*      autoEnabled_;
    QSpinBox*       minWordLen_;
    QDoubleSpinBox* confidence_;

    // Debug tab
    QComboBox*  logLevel_;

    QPushButton* saveBtn_;
    QPushButton* cancelBtn_;
    QPushButton* defaultsBtn_;
};
