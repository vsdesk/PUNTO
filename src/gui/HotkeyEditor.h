#pragma once
#include <QWidget>
#include <QKeySequence>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>

// HotkeyEditor — a small inline widget that lets the user record a key
// combination by pressing it.  Displays the combination as text; Clear
// button resets it.  Emits hotkeyChanged() when the sequence changes.
class HotkeyEditor : public QWidget {
    Q_OBJECT
public:
    explicit HotkeyEditor(QWidget* parent = nullptr);

    QKeySequence keySequence() const;
    void         setKeySequence(const QKeySequence& seq);

    // Fcitx5-style string representation, e.g. "Alt+apostrophe"
    QString fcitxString() const;
    void    setFromFcitxString(const QString& s);

signals:
    void hotkeyChanged(const QKeySequence& seq);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    QLineEdit*   display_;
    QPushButton* clearBtn_;
    QKeySequence current_;
    bool         recording_ = false;

    void setRecording(bool on);
    void updateDisplay();
};
