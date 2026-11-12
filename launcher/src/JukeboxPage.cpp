#include "JukeboxPage.h"

#include "AppPaths.h"
#include "GameData.h"
#include "TitleImage.h"

#include <QApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRandomGenerator>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kTile = 148;       // a music set's picture
constexpr int kPlayWidth = 34;   // a song row's play button, at its left
constexpr int kRole = Qt::UserRole;
constexpr int kPixmapRole = Qt::UserRole + 1, kTitleRole = Qt::UserRole + 2, kModeRole = Qt::UserRole + 3,
              kActiveRole = Qt::UserRole + 4;  // the set tiles
constexpr int kSubRole = Qt::UserRole + 1, kChangeableRole = Qt::UserRole + 2, kHeaderRole = Qt::UserRole + 3;  // slots
constexpr int kFileRole = Qt::UserRole + 1, kLengthRole = Qt::UserRole + 2, kAssignedRole = Qt::UserRole + 3,
              kNoteRole = Qt::UserRole + 4, kPlayingRole = Qt::UserRole + 5, kGameRole = Qt::UserRole + 6;  // songs
constexpr int kSlotFileRole = Qt::UserRole + 7;  // a stage's or menu's song (the overview plays it)

const QColor kText(0xE9, 0xE4, 0xDC), kMuted(0x8E, 0x87, 0x80), kRed(0xE3, 0x1D, 0x2B), kRim(0xE0, 0x30, 0x55);

QFont titleFont(int pixels, bool bold = true) {
    QFont f(QStringLiteral("Bahnschrift"));
    f.setPixelSize(pixels);
    f.setBold(bold);
    return f;
}

// --- the set tiles' pictures ---------------------------------------------------------------------------------

QPixmap fitted(const QImage& img, int size) {
    QPixmap pm(size, size);
    pm.fill(QColor(0x10, 0x10, 0x13));
    if (img.isNull()) return pm;
    const QImage scaled = img.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPainter p(&pm);
    p.drawImage(QPoint(0, 0), scaled, QRect((scaled.width() - size) / 2, (scaled.height() - size) / 2, size, size));
    return pm;
}

// a soundtrack without a picture: its name on a colour of its own
QPixmap namedTile(const QString& big, const QString& small, int hue, int size) {
    QPixmap pm(size, size);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    QLinearGradient g(0, 0, size, size);
    g.setColorAt(0, QColor::fromHsv(hue, 190, 150));
    g.setColorAt(0.55, QColor::fromHsv(hue, 210, 60));
    g.setColorAt(1, QColor(0x0C, 0x0A, 0x0C));
    p.fillRect(pm.rect(), g);
    p.setPen(QColor(255, 255, 255, 26));  // a few stripes, like a cover's art
    for (int x = -size; x < size; x += 14) p.drawLine(x, size, x + size, 0);
    QFont f = titleFont(size / 4);
    const QRect area(8, 8, size - 16, size - 34);
    for (int px = size / 4; px > 10; px -= 2) {
        f.setPixelSize(px);
        const QRect need = QFontMetrics(f).boundingRect(area, Qt::AlignCenter | Qt::TextWordWrap, big);
        if (need.width() <= area.width() && need.height() <= area.height()) break;
    }
    p.setFont(f);
    p.setPen(QColor(0, 0, 0, 140));
    p.drawText(area.translated(2, 2), Qt::AlignCenter | Qt::TextWordWrap, big);
    p.setPen(QColor(0xF4, 0xEE, 0xE6));
    p.drawText(area, Qt::AlignCenter | Qt::TextWordWrap, big);
    p.setFont(titleFont(10, false));
    p.setPen(QColor(255, 255, 255, 170));
    p.drawText(QRect(6, size - 24, size - 12, 18), Qt::AlignCenter,
               QFontMetrics(titleFont(10, false)).elidedText(small, Qt::ElideRight, size - 12));
    return pm;
}

QPixmap soundtrackTile(const jukebox::Soundtrack& s, int size) {
    if (!s.cover.isEmpty()) {
        const QImage img(s.cover);
        if (!img.isNull()) return fitted(img, size);
    }
    return namedTile(s.shortTitle, s.title, int(qHash(s.id) % 360), size);
}

QPixmap playlistTile(int n, int size) {
    QPixmap pm(size, size);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    QLinearGradient g(0, 0, 0, size);
    g.setColorAt(0, QColor(0x5A, 0x5E, 0x68));
    g.setColorAt(1, QColor(0x1C, 0x1E, 0x24));
    p.fillRect(pm.rect(), g);
    p.setFont(titleFont(size * 2 / 3, false));
    p.setPen(QColor(255, 255, 255, 60));
    p.drawText(pm.rect().adjusted(0, -10, 0, 0), Qt::AlignCenter, QStringLiteral("♫"));
    p.setFont(titleFont(15));
    p.setPen(QColor(0xF4, 0xEE, 0xE6));
    p.drawText(pm.rect(), Qt::AlignCenter, QObject::tr("My Playlist %1").arg(n));
    return pm;
}

QPixmap ownTile(const QString& gameId, const QString& title, int size) {
    const QImage logo = titleimage::forGame(gameId);
    if (logo.isNull()) return namedTile(QObject::tr("Original"), title, 352, size);
    QPixmap pm(size, size);
    QPainter p(&pm);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QRadialGradient g(size / 2, size / 2, size * 0.75);
    g.setColorAt(0, QColor(0x3A, 0x08, 0x0E));
    g.setColorAt(1, QColor(0x0C, 0x0C, 0x0E));
    p.fillRect(pm.rect(), g);
    p.drawImage(QRect(8, 4, size - 16, size - 16), logo);
    return pm;
}

QPixmap everyTile(const QList<jukebox::Soundtrack>& library, int size) {
    QPixmap pm(size, size);
    pm.fill(QColor(0x14, 0x10, 0x12));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const int half = size / 2;
    for (int i = 0; i < 4 && i < library.size(); i++)
        p.drawPixmap(QRect((i % 2) * half, (i / 2) * half, half, half), soundtrackTile(library[i], half));
    p.fillRect(pm.rect(), QColor(0x5A, 0x08, 0x12, 120));
    p.setFont(titleFont(30));
    p.setPen(QColor(0, 0, 0, 150));
    p.drawText(pm.rect().translated(2, 2), Qt::AlignCenter, QObject::tr("ALL"));
    p.setPen(QColor(0xF4, 0xEE, 0xE6));
    p.drawText(pm.rect(), Qt::AlignCenter, QObject::tr("ALL"));
    return pm;
}

// --- the delegates -------------------------------------------------------------------------------------------

// a music set: its picture (the current one framed, as Tekken's), a check on the one that plays, its name and mode
class TileDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(kTile + 22, kTile + 52); }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::TextAntialiasing);
        const bool current = opt.state & QStyle::State_Selected;
        const bool hover = opt.state & QStyle::State_MouseOver;
        const QRect img(opt.rect.left() + (opt.rect.width() - kTile) / 2, opt.rect.top() + 6, kTile, kTile);
        p->drawPixmap(img, index.data(kPixmapRole).value<QPixmap>());
        if (!current) p->fillRect(img, QColor(0, 0, 0, hover ? 40 : 95));
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(current ? kRim : hover ? QColor(0x8E, 0x0E, 0x1C) : QColor(0x2C, 0x2C, 0x33), current ? 3 : 1));
        p->drawRect(img.adjusted(1, 1, -1, -1));
        if (index.data(kActiveRole).toBool()) {
            const QRect b(img.left() + 6, img.top() + 6, 18, 18);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0x2F, 0xA8, 0x63));
            p->drawRoundedRect(b, 3, 3);
            p->setPen(QPen(Qt::white, 2.2));
            p->drawPolyline(QPolygon({b.topLeft() + QPoint(4, 9), b.topLeft() + QPoint(8, 13), b.topLeft() + QPoint(14, 5)}));
        }
        const QFont tf = titleFont(13);
        p->setFont(tf);
        p->setPen(current ? QColor(Qt::white) : QColor(0xC9, 0xC1, 0xB8));
        const QRect t(opt.rect.left() + 2, img.bottom() + 6, opt.rect.width() - 4, 18);
        p->drawText(t, Qt::AlignHCenter | Qt::AlignTop,
                    QFontMetrics(tf).elidedText(index.data(kTitleRole).toString(), Qt::ElideRight, t.width()));
        const QFont mf = titleFont(11, false);
        p->setFont(mf);
        p->setPen(current ? QColor(0xF0, 0x9A, 0xA4) : kMuted);
        p->drawText(QRect(t.left(), t.bottom() + 1, t.width(), 16), Qt::AlignHCenter | Qt::AlignTop,
                    index.data(kModeRole).toString());
        p->restore();
    }
};

// a stage or a menu screen: its name and what it plays; the headers between
class SlotDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        return QSize(opt.rect.width(), index.data(kHeaderRole).toBool() ? 30 : 48);
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::TextAntialiasing);
        const QRect r = opt.rect;
        if (index.data(kHeaderRole).toBool()) {
            p->setFont(titleFont(12));
            p->setPen(kRed);
            p->drawText(r.adjusted(10, 0, -6, -4), Qt::AlignLeft | Qt::AlignBottom, index.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }
        const bool current = opt.state & QStyle::State_Selected;
        const bool hover = opt.state & QStyle::State_MouseOver;
        const QRect box = r.adjusted(2, 1, -2, -1);
        if (current) {
            QLinearGradient g(box.topLeft(), box.topRight());
            g.setColorAt(0, QColor(0x9A, 0x12, 0x22));
            g.setColorAt(1, QColor(0x46, 0x08, 0x0F));
            p->fillRect(box, g);
            p->fillRect(QRect(box.left(), box.top(), 4, box.height()), kRim);
        } else {
            p->fillRect(box, hover ? QColor(0x1E, 0x1E, 0x24) : QColor(0x16, 0x16, 0x1A));
        }
        const bool changeable = index.data(kChangeableRole).toBool();
        p->setFont(titleFont(14));
        p->setPen(changeable ? QColor(Qt::white) : kMuted);
        p->drawText(box.adjusted(14, 4, -8, -box.height() / 2), Qt::AlignLeft | Qt::AlignVCenter,
                    index.data(Qt::DisplayRole).toString().toUpper());
        p->setFont(titleFont(12, false));
        p->setPen(current ? QColor(0xF2, 0xC6, 0xCB) : QColor(0xA8, 0xA1, 0x9A));
        const QRect sub = box.adjusted(14, box.height() / 2, -8, -4);
        p->drawText(sub, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(titleFont(12, false)).elidedText(index.data(kSubRole).toString(), Qt::ElideRight, sub.width()));
        p->restore();
    }
};

// a game tab
class TabDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        return QSize(QFontMetrics(titleFont(13)).horizontalAdvance(index.data(Qt::DisplayRole).toString()) + 26, 34);
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        p->save();
        p->setRenderHint(QPainter::TextAntialiasing);
        const bool current = opt.state & QStyle::State_Selected;
        const bool hover = opt.state & QStyle::State_MouseOver;
        p->setFont(titleFont(13, current));
        p->setPen(current ? QColor(Qt::white) : hover ? kText : kMuted);
        p->drawText(opt.rect, Qt::AlignCenter, index.data(Qt::DisplayRole).toString());
        if (current) p->fillRect(QRect(opt.rect.left() + 8, opt.rect.bottom() - 3, opt.rect.width() - 16, 3), kRed);
        p->restore();
    }
};

// a song: its play button, "Title / GAME", what kind it is and how long; the one the stage plays filled, with a check
class SongDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const override { return QSize(opt.rect.width(), 32); }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::TextAntialiasing);
        const QRect r = opt.rect.adjusted(2, 1, -2, -1);
        const bool current = opt.state & QStyle::State_Selected;
        const bool hover = opt.state & QStyle::State_MouseOver;
        const bool assigned = index.data(kAssignedRole).toBool();
        if (assigned) {
            QLinearGradient g(r.topLeft(), r.topRight());
            g.setColorAt(0, QColor(0xB8, 0x14, 0x2A));
            g.setColorAt(1, QColor(0x6A, 0x0A, 0x16));
            p->fillRect(r, g);
        } else {
            p->fillRect(r, current ? QColor(0x2A, 0x14, 0x18) : hover ? QColor(0x1E, 0x1E, 0x24) : QColor(0x15, 0x15, 0x19));
        }
        if (current) {
            p->setPen(QPen(QColor(0xF2, 0xC1, 0x4E), 1));
            p->setBrush(Qt::NoBrush);
            p->drawRect(r.adjusted(0, 0, -1, -1));
        }
        const bool hasFile = !index.data(kFileRole).toString().isEmpty();
        const QRect play(r.left(), r.top(), kPlayWidth, r.height());
        if (hasFile) {
            const bool playing = index.data(kPlayingRole).toBool();
            p->setPen(Qt::NoPen);
            p->setBrush(playing ? QColor(0xF2, 0xC1, 0x4E) : QColor(0xE9, 0xE4, 0xDC, hover || current ? 230 : 150));
            const QPoint c = play.center();
            if (playing) p->drawRect(QRect(c.x() - 5, c.y() - 5, 10, 10));
            else p->drawPolygon(QPolygon({c + QPoint(-4, -6), c + QPoint(7, 0), c + QPoint(-4, 6)}));
        }
        int x = play.right() + 4;
        if (assigned) {
            const QRect b(x, r.center().y() - 8, 16, 16);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0x2F, 0xA8, 0x63));
            p->drawRoundedRect(b, 3, 3);
            p->setPen(QPen(Qt::white, 2));
            p->drawPolyline(QPolygon({b.topLeft() + QPoint(4, 8), b.topLeft() + QPoint(7, 11), b.topLeft() + QPoint(12, 4)}));
            x = b.right() + 8;
        }
        const QString length = index.data(kLengthRole).toString();
        const QString note = index.data(kNoteRole).toString();
        const QFont small = titleFont(11, false);
        const int rightWidth = QFontMetrics(small).horizontalAdvance(note + QStringLiteral("   ") + length) + 12;
        p->setFont(small);
        p->setPen(assigned ? QColor(0xF4, 0xD0, 0xD4) : kMuted);
        p->drawText(QRect(r.right() - rightWidth, r.top(), rightWidth - 8, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                    note.isEmpty() ? length : note + QStringLiteral("   ") + length);
        const QRect text(x, r.top(), r.right() - rightWidth - x, r.height());
        const QString title = index.data(Qt::DisplayRole).toString();
        const QString game = index.data(kGameRole).toString();
        const QFont tf = titleFont(13);
        QFontMetrics fm(tf);
        p->setFont(tf);
        p->setPen(assigned || current ? QColor(Qt::white) : kText);
        const QString shown = fm.elidedText(title, Qt::ElideRight, text.width());
        p->drawText(text, Qt::AlignLeft | Qt::AlignVCenter, shown);
        if (!game.isEmpty() && shown == title) {
            const int w = fm.horizontalAdvance(title);
            p->setFont(titleFont(13, false));
            p->setPen(assigned ? QColor(0xF4, 0xD0, 0xD4) : kMuted);
            const QRect rest(text.left() + w, text.top(), text.width() - w, text.height());
            p->drawText(rest, Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(titleFont(13, false)).elidedText(QStringLiteral(" / ") + game, Qt::ElideRight, rest.width()));
        }
        p->restore();
    }
};

// a stage or menu in the overview of what a set plays: a card of the grid, a square on the one being heard
class OverviewDelegate : public SlotDelegate {
public:
    using SlotDelegate::SlotDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(318, 50); }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        QStyleOptionViewItem o(opt);
        o.state &= ~QStyle::State_Selected;  // nothing is picked here; hover only
        SlotDelegate::paint(p, o, index);
        if (index.data(kPlayingRole).toBool()) {
            p->save();
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0xF2, 0xC1, 0x4E));
            p->drawRect(QRect(opt.rect.right() - 20, opt.rect.center().y() - 5, 10, 10));
            p->restore();
        }
    }
};

QString lengthText(double seconds) {
    if (seconds <= 0) return {};
    const int s = int(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString kindNote(const QString& kind) {
    if (kind == QLatin1String("select")) return QObject::tr("select screen");
    if (kind == QLatin1String("menu")) return QObject::tr("menu");
    if (kind == QLatin1String("theme")) return QObject::tr("theme");
    if (kind == QLatin1String("stinger")) return QObject::tr("sting");
    if (kind == QLatin1String("variant")) return QObject::tr("variant");
    return {};
}

QListWidget* stripList(QListWidget* list) {
    list->setViewMode(QListView::ListMode);
    list->setFlow(QListView::LeftToRight);
    list->setWrapping(false);
    list->setMovement(QListView::Static);
    list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setMouseTracking(true);
    list->viewport()->setAttribute(Qt::WA_Hover);
    list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; outline: none; }"));
    return list;
}

}  // namespace

JukeboxPage::JukeboxPage(QWidget* parent) : Page(parent) {
    loading_ = true;  // until setGame shows a game's settings
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 14, 28, 10);
    root->setSpacing(8);
    root->addWidget(ui::pageHeader(tr("Jukebox"),
                                   tr("The music of any Mortal Kombat in this game, as Tekken 8 does it. Online and in a "
                                      "watched lobby everyone keeps their own choice: the music never touches the match.")));
    views_ = new QStackedWidget;
    root->addWidget(views_, 1);

    // --- the sets, and what the chosen one plays ---
    auto* setsView = new QWidget;
    auto* sv = new QVBoxLayout(setsView);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->setSpacing(8);
    sets_ = stripList(new QListWidget);
    sets_->setItemDelegate(new TileDelegate(sets_));
    sets_->setFixedHeight(kTile + 52 + 18);
    sets_->setSpacing(2);
    sv->addWidget(sets_);
    setHint_ = ui::label(QString(), "Muted");
    setHint_->setWordWrap(true);
    setHint_->setStyleSheet(QStringLiteral("color: #67C6D9;"));  // Tekken's cyan hint line
    sv->addWidget(setHint_);
    QVBoxLayout* ob = nullptr;
    sv->addWidget(ui::card(QString(), &ob), 1);
    auto* otop = new QHBoxLayout;
    overviewTitle_ = ui::label(QString(), "Big");
    otop->addWidget(overviewTitle_);
    otop->addStretch(1);
    edit_ = new QPushButton;
    edit_->setObjectName(QStringLiteral("Primary"));
    edit_->setProperty("compact", true);
    copy_ = new QPushButton(tr("Copy into a playlist"));
    copy_->setToolTip(tr("These songs into My Playlist 1, 2 or 3, to change any of them"));
    otop->addWidget(copy_);
    otop->addWidget(edit_);
    ob->addLayout(otop);
    overview_ = new QListWidget;
    overview_->setItemDelegate(new OverviewDelegate(overview_));
    overview_->setViewMode(QListView::ListMode);
    overview_->setFlow(QListView::LeftToRight);
    overview_->setWrapping(true);
    overview_->setResizeMode(QListView::Adjust);
    overview_->setUniformItemSizes(true);
    overview_->setMovement(QListView::Static);
    overview_->setSpacing(3);
    overview_->setMouseTracking(true);
    overview_->viewport()->setAttribute(Qt::WA_Hover);
    overview_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    overview_->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; }"));
    ob->addWidget(overview_, 1);
    views_->addWidget(setsView);

    // --- a playlist's screen ---
    auto* editView = new QWidget;
    auto* ev = new QVBoxLayout(editView);
    ev->setContentsMargins(0, 0, 0, 0);
    ev->setSpacing(8);
    auto* etop = new QHBoxLayout;
    auto* back = new QPushButton(tr("◀  Back"));
    back->setToolTip(tr("Back to the music sets"));
    editorTitle_ = ui::label(QString(), "Big");
    fill_ = new QPushButton(tr("Fill from a soundtrack"));
    fill_->setToolTip(tr("Every stage and menu of this playlist gets a soundtrack's preset song; then change any"));
    resetAll_ = new QPushButton(tr("Reset all"));
    resetAll_->setToolTip(tr("Every stage and menu of this playlist plays the game's own music again"));
    etop->addWidget(back);
    etop->addSpacing(10);
    etop->addWidget(editorTitle_);
    etop->addStretch(1);
    etop->addWidget(fill_);
    etop->addWidget(resetAll_);
    ev->addLayout(etop);
    auto* panes = new QHBoxLayout;
    panes->setSpacing(14);
    slots_ = new QListWidget;
    slots_->setItemDelegate(new SlotDelegate(slots_));
    slots_->setMouseTracking(true);
    slots_->viewport()->setAttribute(Qt::WA_Hover);
    slots_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    slots_->setMinimumWidth(300);
    slots_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    panes->addWidget(slots_, 5);
    auto* right = new QVBoxLayout;
    right->setSpacing(4);
    tabs_ = stripList(new QListWidget);
    tabs_->setItemDelegate(new TabDelegate(tabs_));
    tabs_->setFixedHeight(48);
    right->addWidget(tabs_);
    tabTitle_ = ui::label(QString(), "Big");
    right->addWidget(tabTitle_);
    tracks_ = new QListWidget;
    tracks_->setItemDelegate(new SongDelegate(tracks_));
    tracks_->setMouseTracking(true);
    tracks_->viewport()->setAttribute(Qt::WA_Hover);
    tracks_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tracks_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    right->addWidget(tracks_, 1);
    panes->addLayout(right, 6);
    ev->addLayout(panes, 1);
    auto* foot = new QHBoxLayout;
    auto* tip = ui::label(tr("You can choose individual songs from each game: pick a stage or menu on the left, then click "
                             "a song (▶ plays it first)."),
                          "Muted");
    tip->setStyleSheet(QStringLiteral("color: #67C6D9;"));
    tip->setWordWrap(true);
    reset_ = new QPushButton(tr("Reset"));
    reset_->setToolTip(tr("The stage or menu picked on the left plays the game's own music again"));
    play_ = new QPushButton(tr("▶  Play"));
    play_->setToolTip(tr("Hear the song picked on the right"));
    foot->addWidget(tip, 1);
    foot->addWidget(reset_);
    foot->addWidget(play_);
    ev->addLayout(foot);
    views_->addWidget(editView);

    // --- both: notes, the music folder, the preparing ---
    notes_ = ui::label(QString(), "Warning");
    notes_->setWordWrap(true);
    notes_->hide();
    root->addWidget(notes_);
    auto* bottom = new QHBoxLayout;
    folder_ = ui::label(QString(), "Muted");
    folder_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* change = new QPushButton(tr("Change"));
    auto* open = new QPushButton(tr("Open folder"));
    auto* rescanButton = new QPushButton(tr("Rescan"));
    status_ = ui::label(QString(), "Muted");
    status_->setWordWrap(true);
    apply_ = new QPushButton(tr("Apply now"));
    bottom->addWidget(folder_);
    bottom->addWidget(change);
    bottom->addWidget(open);
    bottom->addWidget(rescanButton);
    bottom->addSpacing(16);
    bottom->addWidget(status_, 1);
    bottom->addWidget(apply_);
    root->addLayout(bottom);

    // the sets
    connect(sets_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        lastClick_.restart();
        setClicked(item);
    });
    connect(sets_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (lastClick_.isValid() && lastClick_.elapsed() < 600) return;  // the double click's second half
        setClicked(item);
    });
    connect(overview_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        lastClick_.restart();
        if (config_.playlistIndex() >= 0) openEditor(item->data(kRole).toString());
        else togglePreview(item->data(kSlotFileRole).toString());  // what a preset or a shuffle plays, to hear
    });
    connect(overview_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (lastClick_.isValid() && lastClick_.elapsed() < 600) return;
        if (config_.playlistIndex() >= 0) openEditor(item->data(kRole).toString());
        else togglePreview(item->data(kSlotFileRole).toString());
    });
    connect(edit_, &QPushButton::clicked, this, [this] {
        if (config_.playlistIndex() >= 0) openEditor(QString());
    });
    connect(copy_, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        for (int i = 0; i < jukebox::kPlaylists; i++)
            menu.addAction(tr("My Playlist %1").arg(i + 1), this, [this, i] { copyInto(i); });
        menu.exec(copy_->mapToGlobal(QPoint(0, copy_->height())));
    });

    // a playlist's screen
    connect(back, &QPushButton::clicked, this, &JukeboxPage::closeEditor);
    connect(slots_, &QListWidget::currentItemChanged, this, [this] {
        if (!loading_) showSlotSong();
    });
    connect(tabs_, &QListWidget::currentRowChanged, this, [this] {
        if (!loading_) fillTracks();
    });
    connect(tracks_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        lastClick_.restart();
        trackClicked(item, true);
    });
    connect(tracks_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (lastClick_.isValid() && lastClick_.elapsed() < 600) return;
        trackClicked(item, false);
    });
    connect(play_, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem* item = tracks_->currentItem()) togglePreview(item->data(kFileRole).toString());
    });
    connect(reset_, &QPushButton::clicked, this, [this] { assign(QStringLiteral("own")); });
    connect(resetAll_, &QPushButton::clicked, this, [this] {
        const int list = config_.playlistIndex();
        if (list < 0) return;
        config_.playlists[list].clear();
        changed();
    });
    connect(fill_, &QPushButton::clicked, this, [this] {
        const int list = config_.playlistIndex();
        if (list < 0) return;
        QMenu menu(this);
        for (const jukebox::Soundtrack& s : library_) {
            const QString id = s.id;
            menu.addAction(s.title, this, [this, list, id] {
                if (const jukebox::Soundtrack* found = jukebox::find(library_, id)) {
                    config_.playlists[list] = jukebox::presetChoices(gameId_, *found);
                    changed();
                }
            });
        }
        menu.exec(fill_->mapToGlobal(QPoint(0, fill_->height())));
    });

    // the folder and the applying
    connect(change, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Music folder"), jukebox::libraryFolder());
        if (dir.isEmpty()) return;
        jukebox::setLibraryFolder(dir);
        rescan();
        changed();
    });
    connect(open, &QPushButton::clicked, this, [] {
        QDir().mkpath(jukebox::libraryFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(jukebox::libraryFolder()));
    });
    connect(rescanButton, &QPushButton::clicked, this, [this] {
        rescan();
        changed();
    });
    connect(apply_, &QPushButton::clicked, this, [this] {
        applyTimer_.stop();
        jukebox::Service::instance().apply(gameId_);
    });
    applyTimer_.setSingleShot(true);
    applyTimer_.setInterval(1200);
    connect(&applyTimer_, &QTimer::timeout, this, [this] { jukebox::Service::instance().apply(gameId_); });
    jukebox::Service& service = jukebox::Service::instance();
    connect(&service, &jukebox::Service::progress, this, [this](const QString& game, int done, int total, const QString& what) {
        if (game != gameId_) return;
        status_->setText(tr("Preparing %1 of %2: %3").arg(done + 1).arg(total).arg(what));
        apply_->setEnabled(false);
    });
    connect(&service, &jukebox::Service::finished, this, [this](const QString& game, bool ok, const QString& message) {
        applog::write(QStringLiteral("jukebox: %1: %2").arg(game, message));
        if (game != gameId_) return;
        status_->setText(message);
        status_->setObjectName(ok ? QStringLiteral("Muted") : QStringLiteral("Warning"));
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
        apply_->setEnabled(true);
    });
    connect(&service, &jukebox::Service::configChanged, this, [this](const QString& game) {
        if (game != gameId_) return;
        config_ = jukebox::loadConfig(gameId_);  // a shuffle drawn again as the game started
        refresh();
    });
    connect(&preview_, &jukebox::Preview::stateChanged, this, [this](bool playing, const QString& file) {
        for (QListWidget* list : {tracks_, overview_})
            for (int i = 0; i < list->count(); i++) {
                QListWidgetItem* item = list->item(i);
                const int role = list == tracks_ ? kFileRole : kSlotFileRole;
                item->setData(kPlayingRole, playing && !file.isEmpty() && item->data(role).toString() == file);
            }
        play_->setText(playing ? tr("■  Stop") : tr("▶  Play"));
    });

    rescan();
}

void JukeboxPage::showEvent(QShowEvent* event) {
    Page::showEvent(event);
    static bool first = true;
    if (first) {
        first = false;
        return;
    }
    rescan();  // the folder may have new music since
    refresh();
}

void JukeboxPage::rescan() {
    library_ = jukebox::scanLibrary(jukebox::libraryFolder());
    int songs = 0;
    for (const auto& s : library_) songs += int(s.tracks.size());
    folder_->setText(library_.isEmpty()
                         ? tr("No music in %1 yet: put each soundtrack in a folder of its own there.")
                               .arg(QDir::toNativeSeparators(jukebox::libraryFolder()))
                         : tr("%1: %2 soundtrack%3, %4 songs")
                               .arg(QDir::toNativeSeparators(jukebox::libraryFolder()))
                               .arg(library_.size()).arg(library_.size() == 1 ? QString() : QStringLiteral("s"))
                               .arg(songs));
    folder_->setToolTip(tr("Each soundtrack is a folder of its own in the music folder (m4a, mp3, wav or flac files); a "
                           "soundtrack.json in it can name the songs and the select-screen song, and a cover.jpg gives "
                           "its tile a picture; else the file names are used."));
    const bool was = loading_;
    loading_ = true;
    fillSets(false);
    fillTabs();
    loading_ = was;
    if (!loading_) fillTracks();
}

void JukeboxPage::setGame(const QString& gameId) {
    Page::setGame(gameId);
    preview_.stop();
    loading_ = true;
    config_ = jukebox::loadConfig(gameId_);
    fillSets(true);
    fillSlots();
    fillTabs();
    loading_ = false;
    views_->setCurrentIndex(0);
    refresh();
}

void JukeboxPage::fillSets(bool toActive) {
    const QString was = sets_->currentItem() ? sets_->currentItem()->data(kRole).toString() : QString();
    const QSignalBlocker block(sets_);
    sets_->clear();
    const Game* g = GameData::instance().game(gameId_);
    auto add = [this](const QString& key, const QPixmap& pm, const QString& title) {
        auto* item = new QListWidgetItem(sets_);
        item->setData(kRole, key);
        item->setData(kPixmapRole, pm);
        item->setData(kTitleRole, title);
        item->setToolTip(title);
    };
    add(QStringLiteral("own"), ownTile(gameId_, g ? g->title : gameId_, kTile), tr("Original"));
    for (const jukebox::Soundtrack& s : library_) add(QStringLiteral("sound:") + s.id, soundtrackTile(s, kTile), s.title);
    if (!library_.isEmpty()) add(QStringLiteral("all"), everyTile(library_, kTile), tr("Every soundtrack"));
    for (int i = 1; i <= jukebox::kPlaylists; i++)
        add(QStringLiteral("playlist:%1").arg(i), playlistTile(i, kTile), tr("My Playlist %1").arg(i));
    const QString sid = config_.setSoundtrack();
    const QString active = config_.set == QLatin1String("own") ? QStringLiteral("own")
                           : sid == QLatin1String("*")         ? QStringLiteral("all")
                           : !sid.isEmpty()                    ? QStringLiteral("sound:") + sid
                                                               : config_.set;
    const QString want = toActive || was.isEmpty() ? active : was;
    for (int i = 0; i < sets_->count(); i++)
        if (sets_->item(i)->data(kRole).toString().compare(want, Qt::CaseInsensitive) == 0) sets_->setCurrentRow(i);
    if (!sets_->currentItem() && sets_->count()) sets_->setCurrentRow(0);
    sets_->scrollToItem(sets_->currentItem(), QAbstractItemView::PositionAtCenter);
}

void JukeboxPage::fillSlots() {
    const QSignalBlocker block(slots_);
    slots_->clear();
    overview_->clear();
    const jukebox::GameMusic* gm = jukebox::gameMusic(gameId_);
    if (!gm) return;
    auto header = [this](const QString& text) {
        auto* item = new QListWidgetItem(text, slots_);
        item->setData(kHeaderRole, true);
        item->setFlags(Qt::NoItemFlags);
    };
    auto slotItem = [](QListWidget* list, const jukebox::Slot& s) {
        auto* item = new QListWidgetItem(s.name, list);
        item->setData(kRole, s.key);
        item->setData(kChangeableRole, !s.bank.isEmpty());
        item->setToolTip(s.bank.isEmpty() ? QObject::tr("%1 keeps the game's own music: %2").arg(s.name, s.why) : s.name);
    };
    // the editor: the stages, then the menus; the overview: the menus first
    bool stages = false, screens = false;
    for (const jukebox::Slot& s : gm->places) {
        if (s.isStage() && !stages) {
            header(tr("STAGES"));
            stages = true;
        }
        if (!s.isStage() && !screens) {
            header(tr("MENUS"));
            screens = true;
        }
        slotItem(slots_, s);
    }
    for (const jukebox::Slot& s : gm->places)
        if (!s.isStage()) slotItem(overview_, s);
    for (const jukebox::Slot& s : gm->places)
        if (s.isStage()) slotItem(overview_, s);
}

void JukeboxPage::fillTabs() {
    const QString was = tabSoundtrack();
    const QSignalBlocker block(tabs_);
    tabs_->clear();
    auto* own = new QListWidgetItem(tr("Original"), tabs_);
    own->setData(kRole, QString());
    own->setToolTip(tr("The game's own music"));
    for (const jukebox::Soundtrack& s : library_) {
        auto* item = new QListWidgetItem(s.shortTitle, tabs_);
        item->setData(kRole, s.id);
        item->setToolTip(s.title);
    }
    int row = 0;
    for (int i = 0; i < tabs_->count(); i++)
        if (!was.isEmpty() && tabs_->item(i)->data(kRole).toString() == was) row = i;
    tabs_->setCurrentRow(row);
}

QString JukeboxPage::tabSoundtrack() const {
    return tabs_ && tabs_->currentItem() ? tabs_->currentItem()->data(kRole).toString() : QString();
}

QString JukeboxPage::currentSlot() const {
    QListWidgetItem* item = slots_->currentItem();
    return item && !item->data(kHeaderRole).toBool() ? item->data(kRole).toString() : QString();
}

void JukeboxPage::fillTracks() {
    const QString keep = tracks_->currentItem() ? tracks_->currentItem()->data(kRole).toString() : QString();
    tracks_->clear();
    const QString sid = tabSoundtrack();
    const jukebox::Pick* pick = plan_.pick(currentSlot());
    const QString chosen = pick && !pick->file.isEmpty() ? pick->soundtrack + QLatin1Char('/') + QFileInfo(pick->file).fileName()
                                                         : QStringLiteral("own");
    if (sid.isEmpty()) {
        const Game* g = GameData::instance().game(gameId_);
        tabTitle_->setText(tr("ORIGINAL MUSIC"));
        auto* item = new QListWidgetItem(tr("The game's own music"), tracks_);
        item->setData(kRole, QStringLiteral("own"));
        item->setData(kGameRole, g ? g->title : gameId_);
        item->setData(kAssignedRole, chosen == QLatin1String("own"));
    } else if (const jukebox::Soundtrack* s = jukebox::find(library_, sid)) {
        tabTitle_->setText(s->title.toUpper());
        for (const jukebox::Track& t : s->tracks) {
            auto* item = new QListWidgetItem(t.title, tracks_);
            const QString choice = jukebox::choiceOf(*s, t);
            item->setData(kRole, choice);
            item->setData(kFileRole, t.file);
            item->setData(kGameRole, s->shortTitle);
            item->setData(kLengthRole, lengthText(t.seconds));
            item->setData(kNoteRole, kindNote(t.kind));
            item->setData(kAssignedRole, choice.compare(chosen, Qt::CaseInsensitive) == 0);
            item->setData(kPlayingRole, preview_.file() == t.file);
            item->setToolTip(QDir::toNativeSeparators(t.file));
        }
    }
    for (int i = 0; i < tracks_->count(); i++)
        if (tracks_->item(i)->data(kRole).toString() == keep || (keep.isEmpty() && tracks_->item(i)->data(kAssignedRole).toBool()))
            tracks_->setCurrentRow(i);
}

void JukeboxPage::showSlotSong() {
    const jukebox::Pick* pick = plan_.pick(currentSlot());
    const QString sid = pick && !pick->file.isEmpty() ? pick->soundtrack : QString();
    {
        const QSignalBlocker block(tabs_);
        for (int i = 0; i < tabs_->count(); i++)
            if (tabs_->item(i)->data(kRole).toString().compare(sid, Qt::CaseInsensitive) == 0) tabs_->setCurrentRow(i);
    }
    tracks_->setCurrentItem(nullptr);
    fillTracks();
    for (int i = 0; i < tracks_->count(); i++)
        if (tracks_->item(i)->data(kAssignedRole).toBool()) {
            tracks_->setCurrentRow(i);
            tracks_->scrollToItem(tracks_->item(i), QAbstractItemView::PositionAtCenter);
        }
    tabs_->scrollToItem(tabs_->currentItem());
}

void JukeboxPage::refresh() {
    plan_ = jukebox::resolve(gameId_, config_, library_);
    const QString sid = config_.setSoundtrack();
    const int list = config_.playlistIndex();
    const jukebox::Soundtrack* soundtrack = sid.isEmpty() || sid == QLatin1String("*") ? nullptr : jukebox::find(library_, sid);

    // the tiles: which plays, and how
    for (int i = 0; i < sets_->count(); i++) {
        QListWidgetItem* item = sets_->item(i);
        const QString key = item->data(kRole).toString();
        bool active = false;
        QString mode;
        if (key == QLatin1String("own")) {
            active = config_.set == QLatin1String("own");
            mode = tr("The game's own");
        } else if (key == QLatin1String("all")) {
            active = config_.set == QLatin1String("shuffle:*");
            mode = tr("Shuffle");
        } else if (key.startsWith(QLatin1String("sound:"))) {
            active = sid.compare(key.mid(6), Qt::CaseInsensitive) == 0;
            mode = !active ? tr("Preset · Shuffle") : config_.set.startsWith(QLatin1String("shuffle:")) ? tr("Shuffle") : tr("Preset");
        } else {
            active = config_.set == key;
            mode = tr("Playlist");
        }
        item->setData(kActiveRole, active);
        item->setData(kModeRole, mode);
    }

    QString hint;
    if (config_.set == QLatin1String("own")) hint = tr("The game's own music, everywhere.");
    else if (config_.set == QLatin1String("shuffle:*"))
        hint = tr("Play while listening to music from every soundtrack in the folder, shuffled: new songs each time the game starts.");
    else if (list >= 0)
        hint = tr("The background music is set to My Playlist %1: a song from any game for each stage and menu.").arg(list + 1);
    else if (config_.set.startsWith(QLatin1String("shuffle:")))
        hint = tr("Play the game while listening to the music from %1, shuffled: new songs each time the game starts.")
                   .arg(soundtrack ? soundtrack->title : sid);
    else
        hint = tr("Play the game while listening to the music from %1: each stage gets the song that fits it best, the "
                  "menus its own menu and select-screen music.").arg(soundtrack ? soundtrack->title : sid);
    setHint_->setText(hint);
    overviewTitle_->setText(jukebox::describeSet(config_.set, library_).toUpper());
    edit_->setVisible(list >= 0);
    edit_->setText(tr("Edit My Playlist %1").arg(list + 1));
    copy_->setVisible(list < 0 && config_.set != QLatin1String("own"));
    overview_->setToolTip(list >= 0 ? tr("Click a stage or menu to pick its song") : tr("Click one to hear its song"));
    editorTitle_->setText(list >= 0 ? tr("MY PLAYLIST %1").arg(list + 1) : QString());

    // what each stage and menu plays
    for (QListWidget* view : {slots_, overview_})
        for (int i = 0; i < view->count(); i++) {
            QListWidgetItem* item = view->item(i);
            if (item->data(kHeaderRole).toBool()) continue;
            const jukebox::Pick* p = plan_.pick(item->data(kRole).toString());
            QString sub;
            if (!item->data(kChangeableRole).toBool()) {
                sub = tr("Original music (cannot be changed yet)");
            } else if (!p || p->file.isEmpty()) {
                sub = tr("Original music");
            } else {
                const jukebox::Soundtrack* s = jukebox::find(library_, p->soundtrack);
                sub = QStringLiteral("%1 / %2").arg(p->title, s ? s->shortTitle : p->soundtrack);
            }
            item->setData(kSubRole, sub);
            item->setData(kSlotFileRole, p ? p->file : QString());
            item->setData(kPlayingRole, p && !p->file.isEmpty() && preview_.file() == p->file);
        }
    fillTracks();
    sets_->viewport()->update();
    slots_->viewport()->update();
    overview_->viewport()->update();
    notes_->setText(plan_.notes.join(QStringLiteral("\n")));
    notes_->setVisible(!plan_.notes.isEmpty());
}

void JukeboxPage::setClicked(QListWidgetItem* item) {
    if (!item) return;
    const QString key = item->data(kRole).toString();
    if (key == QLatin1String("own")) {
        chooseSet(QStringLiteral("own"));
    } else if (key == QLatin1String("all")) {
        chooseSet(QStringLiteral("shuffle:*"));
    } else if (key.startsWith(QLatin1String("playlist:"))) {
        const bool again = config_.set == key;
        chooseSet(key);
        if (again) openEditor(QString());  // as Tekken's: a playlist chosen again opens it
    } else if (key.startsWith(QLatin1String("sound:"))) {
        // as Tekken's: the soundtrack's preset, or its songs shuffled
        const QString id = key.mid(6);
        QMenu menu(this);
        QAction* preset = menu.addAction(tr("Preset: each stage gets the song that fits it best"));
        QAction* shuffle = menu.addAction(tr("Shuffle: new songs each time the game starts"));
        preset->setCheckable(true);
        shuffle->setCheckable(true);
        preset->setChecked(config_.set.compare(QStringLiteral("preset:") + id, Qt::CaseInsensitive) == 0);
        shuffle->setChecked(config_.set.compare(QStringLiteral("shuffle:") + id, Qt::CaseInsensitive) == 0);
        menu.setActiveAction(shuffle->isChecked() ? shuffle : preset);
        const QRect r = sets_->visualItemRect(item);
        QAction* chosen = menu.exec(sets_->viewport()->mapToGlobal(QPoint(r.left() + 16, r.top() + kTile - 12)));
        if (chosen == preset) chooseSet(QStringLiteral("preset:") + id);
        else if (chosen == shuffle) chooseSet(QStringLiteral("shuffle:") + id);
    }
}

void JukeboxPage::chooseSet(const QString& set) {
    if (set.startsWith(QLatin1String("shuffle:")) && config_.set != set)
        config_.shuffleDraw = QRandomGenerator::global()->generate() | 1u;
    config_.set = set;
    changed();
}

void JukeboxPage::openEditor(const QString& slot) {
    if (config_.playlistIndex() < 0) return;
    views_->setCurrentIndex(1);
    int row = -1;
    for (int i = 0; i < slots_->count(); i++) {
        QListWidgetItem* item = slots_->item(i);
        if (item->data(kHeaderRole).toBool()) continue;
        if (row < 0 && slot.isEmpty()) row = i;
        if (item->data(kRole).toString() == slot) row = i;
    }
    if (row >= 0) {
        const QSignalBlocker block(slots_);
        slots_->setCurrentRow(row);
        slots_->scrollToItem(slots_->item(row), QAbstractItemView::PositionAtCenter);
    }
    showSlotSong();
    slots_->setFocus();
}

void JukeboxPage::closeEditor() {
    views_->setCurrentIndex(0);
    sets_->setFocus();
}

void JukeboxPage::trackClicked(QListWidgetItem* item, bool fromMouse) {
    if (!item) return;
    const QString file = item->data(kFileRole).toString();
    if (fromMouse && !file.isEmpty()) {
        const QRect r = tracks_->visualItemRect(item);
        const QPoint at = tracks_->viewport()->mapFromGlobal(QCursor::pos());
        if (at.x() < r.left() + kPlayWidth + 4) {  // its play button
            togglePreview(file);
            return;
        }
    }
    assign(item->data(kRole).toString());
}

void JukeboxPage::assign(const QString& choice) {
    const int list = config_.playlistIndex();
    const QString key = currentSlot();
    const jukebox::GameMusic* gm = jukebox::gameMusic(gameId_);
    const jukebox::Slot* slot = gm ? gm->slot(key) : nullptr;
    if (list < 0 || !slot) return;
    if (slot->bank.isEmpty() && choice != QLatin1String("own")) {
        status_->setText(tr("%1 keeps the game's own music: %2.").arg(slot->name, slot->why));
        return;
    }
    if (choice == QLatin1String("own")) config_.playlists[list].remove(key);
    else config_.playlists[list].insert(key, choice);
    changed();
}

void JukeboxPage::copyInto(int playlist) {
    QMap<QString, QString> choices;
    for (const jukebox::Pick& p : plan_.picks)
        if (!p.file.isEmpty()) choices.insert(p.slot, p.soundtrack + QLatin1Char('/') + QFileInfo(p.file).fileName());
    config_.playlists[playlist] = choices;
    config_.set = QStringLiteral("playlist:%1").arg(playlist + 1);
    {
        const QSignalBlocker block(sets_);
        for (int i = 0; i < sets_->count(); i++)
            if (sets_->item(i)->data(kRole).toString() == config_.set) sets_->setCurrentRow(i);
    }
    changed();
    openEditor(QString());
}

void JukeboxPage::changed() {
    jukebox::saveConfig(gameId_, config_);
    refresh();
    status_->setText(tr("Applying in a moment…"));
    applyTimer_.start();
}

void JukeboxPage::togglePreview(const QString& file) {
    if (file.isEmpty()) return;
    if (preview_.file() == file) {
        preview_.stop();
        return;
    }
    preview_.play(file);
}
