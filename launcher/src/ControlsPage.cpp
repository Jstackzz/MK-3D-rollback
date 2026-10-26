#include "ControlsPage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

#include "WinUtil.h"

namespace {

// Waits for one key, Tab, Enter and Space included: accepted with that key, done(kNoKey) to take the key away, rejected
// by Esc or Cancel.
class KeyDialog : public QDialog {
public:
    static constexpr int kNoKey = 2;

    KeyDialog(const QString& button, const QString& now, QWidget* parent) : QDialog(parent) {
        setWindowTitle(tr("Change key"));
        setModal(true);
        setMinimumWidth(380);
        setFocusPolicy(Qt::StrongFocus);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(24, 20, 24, 18);
        v->setSpacing(12);
        v->addWidget(ui::label(tr("Press the key for %1").arg(button), "Big"));
        hint_ = ui::label(now.isEmpty() ? tr("Esc cancels.") : tr("It is %1 now. Esc cancels.").arg(now), "Muted");
        hint_->setWordWrap(true);
        v->addWidget(hint_);
        auto* row = new QHBoxLayout;
        row->addStretch();
        auto* none = new QPushButton(tr("No key"));
        auto* cancel = new QPushButton(tr("Cancel"));
        for (QPushButton* b : {none, cancel}) {
            b->setFocusPolicy(Qt::NoFocus);  // every key comes to the dialog
            b->setAutoDefault(false);
            row->addWidget(b);
        }
        v->addLayout(row);
        connect(none, &QPushButton::clicked, this, [this] { done(kNoKey); });
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    }
    int key() const { return key_; }

protected:
    void showEvent(QShowEvent* event) override {
        QDialog::showEvent(event);
        setFocus();
    }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (event->type() != QEvent::KeyPress) return QDialog::event(event);
        const auto* k = static_cast<QKeyEvent*>(event);
        if (k->isAutoRepeat()) return true;
        if (k->key() == Qt::Key_Escape) {
            reject();
        } else if (const int vk = controls::keyFromEvent(k)) {
            key_ = vk;
            accept();
        } else {
            QString name = QKeySequence(k->key()).toString(QKeySequence::NativeText);
            if (name.isEmpty()) name = tr("That key");
            hint_->setText(tr("%1 is kept for Windows or the game; press another key. Esc cancels.").arg(name));
        }
        return true;
    }

private:
    QLabel* hint_;
    int key_ = 0;
};

QString keyText(const QList<int>& keys, int slot) {
    return slot < keys.size() ? controls::keyLabel(keys[slot]) : QStringLiteral("—");
}

}  // namespace

ControlsPage::ControlsPage(QWidget* parent) : Page(parent) {
    using namespace controls;
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(28, 20, 28, 20);
    root->setSpacing(10);
    root->addWidget(ui::pageHeader(
        tr("Controls"), tr("For all three games, on this PC: they only change what your own keys and buttons press, so "
                           "they work the same in online matches. A game that is running picks up a change within a "
                           "second.")));
    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    root->addLayout(columns);

    // keyboard: each Xbox button and its keys
    auto* keyBox = new QGroupBox(tr("KEYBOARD"));
    auto* kv = new QVBoxLayout(keyBox);
    keyboard_ = new QCheckBox(tr("Play with the keyboard as player 1 (while the game's window is in front)"));
    kv->addWidget(keyboard_);
    auto* keyGrid = new QGridLayout;
    keyGrid->setHorizontalSpacing(10);
    keyGrid->setVerticalSpacing(4);
    keyGrid->addWidget(ui::label(tr("Xbox button"), "Muted"), 0, 0);
    keyGrid->addWidget(ui::label(tr("Key"), "Muted"), 0, 1);
    keyGrid->addWidget(ui::label(tr("Second key"), "Muted"), 0, 2);
    for (int c = 0; c < Count; ++c) {
        keyGrid->addWidget(new QLabel(controlLabel(c)), c + 1, 0);
        for (int slot = 0; slot < kKeysPerButton; ++slot) {
            auto* b = new QPushButton;
            b->setMinimumWidth(130);
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(tr("Click, then press the key for %1").arg(controlLabel(c)));
            connect(b, &QPushButton::clicked, this, [this, c, slot] { pickKey(c, slot); });
            keyButtons_[c][slot] = b;
            keyGrid->addWidget(b, c + 1, slot + 1);
        }
    }
    keyGrid->setColumnStretch(0, 1);
    kv->addLayout(keyGrid);
    auto* keyNote = ui::label(tr("Click a key to change it. A key does one thing: giving it to another button takes it "
                                 "from the one that had it."), "Muted");
    keyNote->setWordWrap(true);
    kv->addWidget(keyNote);
    auto* resetKeys = new QPushButton(tr("Default keys"));
    auto* keyButtonsRow = new QHBoxLayout;
    keyButtonsRow->addWidget(resetKeys);
    keyButtonsRow->addStretch();
    kv->addLayout(keyButtonsRow);
    kv->addStretch();
    columns->addWidget(keyBox, 1);

    // controllers: each button of a PC controller and what it presses
    auto* padBox = new QGroupBox(tr("CONTROLLER"));
    auto* pv = new QVBoxLayout(padBox);
    auto* padNote = ui::label(tr("What each button of your controller presses, for every controller plugged in: Xbox, "
                                 "PlayStation and Switch controllers, their buttons named by where they sit."), "Muted");
    padNote->setWordWrap(true);
    pv->addWidget(padNote);
    auto* padGrid = new QGridLayout;
    padGrid->setHorizontalSpacing(10);
    padGrid->setVerticalSpacing(4);
    padGrid->addWidget(ui::label(tr("Your controller"), "Muted"), 0, 0);
    padGrid->addWidget(ui::label(tr("Presses"), "Muted"), 0, 1);
    const QList<Source>& all = sources();
    for (int i = 0; i < all.size(); ++i) {
        auto* name = new QLabel(all[i].label);
        name->setToolTip(all[i].hint);
        auto* box = new QComboBox;
        box->setMinimumWidth(170);
        box->addItem(tr("Nothing"), -1);
        for (int c = 0; c < Count; ++c) box->addItem(controlLabel(c), c);
        box->setToolTip(all[i].hint);
        connect(box, &QComboBox::currentIndexChanged, this, [this, i, box] {
            map_.presses[i] = box->currentData().toInt();
            store();
            showMap();
        });
        pressBoxes_ << box;
        padGrid->addWidget(name, i + 1, 0);
        padGrid->addWidget(box, i + 1, 1);
    }
    padGrid->setColumnStretch(0, 1);
    pv->addLayout(padGrid);
    unpressed_ = ui::label(QString(), "Warning");
    unpressed_->setWordWrap(true);
    pv->addWidget(unpressed_);
    auto* resetPad = new QPushButton(tr("Default buttons"));
    auto* padButtonsRow = new QHBoxLayout;
    padButtonsRow->addWidget(resetPad);
    padButtonsRow->addStretch();
    pv->addLayout(padButtonsRow);
    pv->addStretch();
    columns->addWidget(padBox, 1);

    auto* game = ui::label(tr("Which button punches, kicks or blocks is set in each game's own controller options. "
                              "Deadly Alliance and Deception keep yours online; Armageddon plays online matches with "
                              "its default layout, so change what your buttons press here instead."), "Muted");
    game->setWordWrap(true);
    root->addWidget(game);
    status_ = ui::label(QString(), "Muted");
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(status_);
    root->addStretch();

    connect(keyboard_, &QCheckBox::toggled, this, [this](bool on) {
        map_.keyboard = on;
        store();
        showMap();
    });
    connect(resetKeys, &QPushButton::clicked, this, [this] {
        const Map d = defaults();
        map_.keys = d.keys;
        map_.keyboard = true;
        store();
        showMap();
        say(tr("The keyboard has its default keys again."));
    });
    connect(resetPad, &QPushButton::clicked, this, [this] {
        map_.presses = defaults().presses;
        store();
        showMap();
        say(tr("The controller has its default buttons again."));
    });

    map_ = load();
    showMap();
}

void ControlsPage::showEvent(QShowEvent* event) {
    Page::showEvent(event);
    map_ = controls::load();  // someone may have edited the file
    showMap();
    say(tr("Kept in %1").arg(QDir::toNativeSeparators(controls::file())));
}

void ControlsPage::showMap() {
    using namespace controls;
    {
        const QSignalBlocker block(keyboard_);
        keyboard_->setChecked(map_.keyboard);
    }
    for (int c = 0; c < Count; ++c)
        for (int slot = 0; slot < kKeysPerButton; ++slot) {
            QPushButton* b = keyButtons_[c][slot];
            b->setText(keyText(map_.keys[c], slot));
            // the second key once there is a first
            b->setEnabled(map_.keyboard && slot <= map_.keys[c].size());
        }
    QStringList unpressed;
    for (int c = 0; c < Count; ++c)
        if (!map_.presses.contains(c)) unpressed << controlLabel(c);
    for (int i = 0; i < pressBoxes_.size(); ++i) {
        const QSignalBlocker block(pressBoxes_[i]);
        pressBoxes_[i]->setCurrentIndex(qMax(0, pressBoxes_[i]->findData(map_.presses.value(i, -1))));
    }
    unpressed_->setText(unpressed.isEmpty() ? QString()
                                            : tr("No button of the controller presses %1.").arg(unpressed.join(QStringLiteral(", "))));
    unpressed_->setVisible(!unpressed.isEmpty());
}

void ControlsPage::pickKey(int control, int slot) {
    using namespace controls;
    const QList<int>& before = map_.keys[control];
    KeyDialog dlg(controlLabel(control), slot < before.size() ? keyLabel(before[slot]) : QString(), this);
    winutil::useDarkTitleBar(&dlg);
    const int result = dlg.exec();
    QList<int>& keys = map_.keys[control];
    if (result == KeyDialog::kNoKey) {
        if (slot < keys.size()) keys.removeAt(slot);
        store();
        showMap();
        say(tr("%1 has one key less.").arg(controlLabel(control)));
        return;
    }
    if (result != QDialog::Accepted) return;
    const int vk = dlg.key();
    QString tookFrom;
    for (int c = 0; c < Count; ++c)
        if (c != control && map_.keys[c].removeAll(vk) > 0) tookFrom = controlLabel(c);
    const int at = qMin(slot, int(keys.size()));
    if (at < keys.size()) keys[at] = vk;
    else keys.append(vk);
    for (int i = 0; i < keys.size(); ++i)  // it was this button's other key
        if (i != at && keys[i] == vk) {
            keys.removeAt(i);
            break;
        }
    store();
    showMap();
    if (tookFrom.isEmpty()) say(tr("%1 presses %2.").arg(keyLabel(vk), controlLabel(control)));
    else say(tr("%1 presses %2 now, no longer %3.").arg(keyLabel(vk), controlLabel(control), tookFrom));
}

void ControlsPage::store() {
    QString error;
    if (!controls::save(map_, &error)) say(error, true);
}

void ControlsPage::say(const QString& text, bool warn) {
    status_->setObjectName(warn ? QStringLiteral("Warning") : QStringLiteral("Muted"));
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
    status_->setText(text);
}
