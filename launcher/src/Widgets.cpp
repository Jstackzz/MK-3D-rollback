#include "Widgets.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QVBoxLayout>

namespace ui {

QFrame* card(const QString& title, QVBoxLayout** body, QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("Card"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(10);
    if (!title.isEmpty()) {
        auto* t = label(title.toUpper(), "CardTitle");
        letterSpacing(t, 2);
        layout->addWidget(t);
    }
    if (body) *body = layout;
    return frame;
}

QLabel* label(const QString& text, const char* objectName, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    if (objectName) l->setObjectName(QLatin1String(objectName));
    return l;
}

QWidget* pageHeader(const QString& title, const QString& subtitle, QLabel** subtitleOut) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 4);
    v->setSpacing(2);
    auto* t = label(title.toUpper(), "PageTitle");
    letterSpacing(t, 4);
    auto* s = label(subtitle, "PageSubtitle");
    s->setWordWrap(true);
    v->addWidget(t);
    v->addWidget(s);
    if (subtitleOut) *subtitleOut = s;
    return w;
}

void letterSpacing(QWidget* w, qreal pixels) {
    QFont f = w->font();
    f.setLetterSpacing(QFont::AbsoluteSpacing, pixels);
    w->setFont(f);
}

void fillRegions(QComboBox* box, const QString& select) {
    box->clear();
    for (const auto& [id, name] : GameData::regions()) box->addItem(name, id);
    const int i = box->findData(select);
    box->setCurrentIndex(i >= 0 ? i : 0);
}

void fillMaxPing(QComboBox* box, int select) {
    box->clear();
    for (int ms : {60, 100, 150, 200}) box->addItem(QStringLiteral("%1 ms").arg(ms), ms);
    box->addItem(QObject::tr("Any"), 0);
    const int i = box->findData(select);
    box->setCurrentIndex(i >= 0 ? i : 2);
}

QWidget* statTile(const QString& caption, QLabel** valueOut) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* value = label(QStringLiteral("-"), "StatValue");
    auto* cap = label(caption.toUpper(), "StatLabel");
    letterSpacing(cap, 2);
    v->addWidget(value);
    v->addWidget(cap);
    if (valueOut) *valueOut = value;
    return w;
}

}  // namespace ui

CharacterPicker::CharacterPicker(QWidget* parent) : QListWidget(parent) {
    setObjectName(QStringLiteral("CharacterGrid"));
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setUniformItemSizes(true);
    setGridSize(QSize(138, 42));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMinimumHeight(200);
    connect(this, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item) emit picked(item->data(Qt::UserRole).toString());
    });
}

void CharacterPicker::setRoster(const QList<Character>& roster, const QString& select) {
    const QSignalBlocker block(this);
    clear();
    for (const auto& c : roster) {
        auto* item = new QListWidgetItem(c.name, this);
        item->setData(Qt::UserRole, c.id);
        item->setTextAlignment(Qt::AlignCenter);
        item->setSizeHint(QSize(130, 36));
        if (c.secret) item->setToolTip(tr("%1: unlockable in the game").arg(c.name));
    }
    selectId(select);
    if (!currentItem() && count() > 0) setCurrentRow(0);
}

QString CharacterPicker::currentId() const {
    return currentItem() ? currentItem()->data(Qt::UserRole).toString() : QString();
}

QString CharacterPicker::currentName() const { return currentItem() ? currentItem()->text() : QString(); }

void CharacterPicker::selectId(const QString& id) {
    for (int i = 0; i < count(); ++i)
        if (item(i)->data(Qt::UserRole).toString() == id) {
            setCurrentRow(i);
            scrollToItem(item(i));
            return;
        }
}

LogView::LogView(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setMaximumBlockCount(500);
    setMinimumHeight(90);
    QFont f(QStringLiteral("Consolas"));
    f.setPointSize(9);
    setFont(f);
}

void LogView::line(const QString& text) {
    appendPlainText(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")), text));
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}
