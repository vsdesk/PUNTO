#include "HotkeyEditor.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QMap>

// Mapping from Qt::Key to Fcitx5 key name strings.
// Qt::Key_QuoteLeft (0x60) is the backtick/grave key (` / ~).
// QMap does not support brace-init in all compilers; use a function instead.
static QMap<int, QString> buildQtToFcitxNames() {
    QMap<int, QString> m;
    m[Qt::Key_Space]        = "space";
    m[Qt::Key_Apostrophe]   = "apostrophe";
    m[Qt::Key_Semicolon]    = "semicolon";
    m[Qt::Key_Comma]        = "comma";
    m[Qt::Key_Period]       = "period";
    m[Qt::Key_Slash]        = "slash";
    m[Qt::Key_Backslash]    = "backslash";
    m[Qt::Key_BracketLeft]  = "bracketleft";
    m[Qt::Key_BracketRight] = "bracketright";
    m[Qt::Key_Minus]        = "minus";
    m[Qt::Key_Equal]        = "equal";
    m[Qt::Key_QuoteLeft]    = "grave";  // backtick / grave accent
    m[Qt::Key_Tab]          = "Tab";
    m[Qt::Key_Return]       = "Return";
    m[Qt::Key_Escape]       = "Escape";
    m[Qt::Key_Backspace]    = "BackSpace";
    m[Qt::Key_Delete]       = "Delete";
    m[Qt::Key_F1]  = "F1";  m[Qt::Key_F2]  = "F2";  m[Qt::Key_F3]  = "F3";
    m[Qt::Key_F4]  = "F4";  m[Qt::Key_F5]  = "F5";  m[Qt::Key_F6]  = "F6";
    m[Qt::Key_F7]  = "F7";  m[Qt::Key_F8]  = "F8";  m[Qt::Key_F9]  = "F9";
    m[Qt::Key_F10] = "F10"; m[Qt::Key_F11] = "F11"; m[Qt::Key_F12] = "F12";
    return m;
}
static const QMap<int, QString> QT_TO_FCITX_NAMES = buildQtToFcitxNames();

HotkeyEditor::HotkeyEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    display_ = new QLineEdit(this);
    display_->setReadOnly(true);
    display_->setPlaceholderText(tr("Click to record hotkey…"));
    display_->setFocusPolicy(Qt::StrongFocus);
    display_->installEventFilter(this);

    clearBtn_ = new QPushButton(tr("Clear"), this);
    clearBtn_->setMaximumWidth(60);

    layout->addWidget(display_);
    layout->addWidget(clearBtn_);

    connect(clearBtn_, &QPushButton::clicked, this, [this]() {
        current_ = QKeySequence();
        updateDisplay();
        emit hotkeyChanged(current_);
    });

    // Clicking the display puts us in recording mode
    connect(display_, &QLineEdit::selectionChanged, this, [this]() {
        if (!recording_) setRecording(true);
    });
    display_->setFocusPolicy(Qt::ClickFocus);
    setFocusProxy(display_);
}

QKeySequence HotkeyEditor::keySequence() const { return current_; }

void HotkeyEditor::setKeySequence(const QKeySequence& seq) {
    current_ = seq;
    updateDisplay();
}

QString HotkeyEditor::fcitxString() const {
    if (current_.isEmpty()) return QString();

    // Extract the single-key combination from the QKeySequence
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QKeyCombination combo = current_[0];
    Qt::KeyboardModifiers mods = combo.keyboardModifiers();
    Qt::Key key = combo.key();
#else
    int raw = current_[0];
    Qt::KeyboardModifiers mods = Qt::KeyboardModifiers(raw & Qt::MODIFIER_MASK);
    Qt::Key key = Qt::Key(raw & ~Qt::MODIFIER_MASK);
#endif

    QStringList parts;
    if (mods & Qt::ControlModifier) parts << "ctrl";
    if (mods & Qt::AltModifier)     parts << "alt";
    if (mods & Qt::MetaModifier)    parts << "super";
    if (mods & Qt::ShiftModifier)   parts << "shift";

    // Key name
    if (QT_TO_FCITX_NAMES.contains(key)) {
        parts << QT_TO_FCITX_NAMES[key];
    } else if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        // Lowercase letter
        parts << QString(QChar('a' + (key - Qt::Key_A)));
    } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        parts << QString(QChar('0' + (key - Qt::Key_0)));
    } else {
        parts << QKeySequence(key).toString().toLower();
    }

    return parts.join('+');
}

void HotkeyEditor::setFromFcitxString(const QString& s) {
    if (s.isEmpty()) { current_ = QKeySequence(); updateDisplay(); return; }

    QStringList parts = s.toLower().split('+');
    Qt::KeyboardModifiers mods;
    Qt::Key key = Qt::Key_unknown;

    for (const QString& p : parts) {
        if (p == "ctrl")  mods |= Qt::ControlModifier;
        else if (p == "alt")   mods |= Qt::AltModifier;
        else if (p == "shift") mods |= Qt::ShiftModifier;
        else if (p == "super" || p == "meta") mods |= Qt::MetaModifier;
        else {
            // Try to find the key
            bool found = false;
            for (auto it = QT_TO_FCITX_NAMES.cbegin(); it != QT_TO_FCITX_NAMES.cend(); ++it) {
                if (it.value().toLower() == p) {
                    key = static_cast<Qt::Key>(it.key());
                    found = true;
                    break;
                }
            }
            if (!found && p.length() == 1) {
                QChar c = p[0];
                if (c >= 'a' && c <= 'z') key = Qt::Key(Qt::Key_A + (c.toLatin1() - 'a'));
                else if (c >= '0' && c <= '9') key = Qt::Key(Qt::Key_0 + (c.toLatin1() - '0'));
            }
        }
    }

    if (key != Qt::Key_unknown) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        current_ = QKeySequence(QKeyCombination(mods, key));
#else
        current_ = QKeySequence(static_cast<int>(mods) | static_cast<int>(key));
#endif
    } else {
        current_ = QKeySequence();
    }
    updateDisplay();
}

void HotkeyEditor::keyPressEvent(QKeyEvent* event) {
    if (!recording_) {
        QWidget::keyPressEvent(event);
        return;
    }

    // Ignore lone modifier presses
    int key = event->key();
    if (key == Qt::Key_Control || key == Qt::Key_Alt ||
        key == Qt::Key_Shift   || key == Qt::Key_Meta ||
        key == Qt::Key_AltGr)
    {
        return;
    }

    // Escape cancels recording
    if (key == Qt::Key_Escape) {
        setRecording(false);
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    current_ = QKeySequence(QKeyCombination(event->modifiers(), Qt::Key(key)));
#else
    current_ = QKeySequence(static_cast<int>(event->modifiers()) | key);
#endif
    setRecording(false);
    updateDisplay();
    emit hotkeyChanged(current_);
}

void HotkeyEditor::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    setRecording(true);
}

void HotkeyEditor::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    setRecording(false);
}

void HotkeyEditor::setRecording(bool on) {
    recording_ = on;
    if (on) {
        display_->setStyleSheet("background: #fffde7; border: 1px solid #f9a825;");
        display_->setPlaceholderText(tr("Press key combination…"));
    } else {
        display_->setStyleSheet(QString());
        display_->setPlaceholderText(tr("Click to record hotkey…"));
    }
}

void HotkeyEditor::updateDisplay() {
    display_->setText(current_.toString(QKeySequence::NativeText));
}
