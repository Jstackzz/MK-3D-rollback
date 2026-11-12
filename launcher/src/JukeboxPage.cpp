#include "JukeboxPage.h"

#include "AppPaths.h"
#include "GameData.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kAutoRow = 0, kOwnRow = 1;

}  // namespace

JukeboxPage::JukeboxPage(QWidget* parent) : Page(parent) {
    loading_ = true;  // until setGame shows a game's settings: a combo given its model would otherwise count as a change
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 20, 28, 16);
    root->setSpacing(14);
    root->addWidget(ui::pageHeader(tr("Jukebox"),
                                   tr("The music of any Mortal Kombat on this game's stages, as Tekken 8 does it: pick a "
                                      "soundtrack and every stage gets a song of it, the select screen that game's select "
                                      "music; or pick a song for any stage from any soundtrack. Online and in a watched "
                                      "lobby everyone keeps their own choice: the music never touches the match.")));

    // the music folder
    QVBoxLayout* lib = nullptr;
    root->addWidget(ui::card(tr("Music folder"), &lib));
    auto* folderRow = new QHBoxLayout;
    folder_ = ui::label(QString());
    folder_->setWordWrap(true);
    auto* change = new QPushButton(tr("Change"));
    auto* open = new QPushButton(tr("Open folder"));
    auto* rescanButton = new QPushButton(tr("Rescan"));
    folderRow->addWidget(folder_, 1);
    folderRow->addWidget(change);
    folderRow->addWidget(open);
    folderRow->addWidget(rescanButton);
    lib->addLayout(folderRow);
    libraryInfo_ = ui::label(QString(), "Muted");
    libraryInfo_->setWordWrap(true);
    lib->addWidget(libraryInfo_);
    connect(change, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Music folder"), jukebox::libraryFolder());
        if (dir.isEmpty()) return;
        jukebox::setLibraryFolder(dir);
        rescan();
        showConfig();
        status_->setText(tr("Applying in a moment…"));
        applyTimer_.start();
    });
    connect(open, &QPushButton::clicked, this, [] {
        QDir().mkpath(jukebox::libraryFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(jukebox::libraryFolder()));
    });
    connect(rescanButton, &QPushButton::clicked, this, [this] {
        rescan();
        showConfig();
        status_->setText(tr("Applying in a moment…"));
        applyTimer_.start();
    });

    // the soundtrack and the select screen
    QVBoxLayout* pick = nullptr;
    root->addWidget(ui::card(tr("Soundtrack"), &pick));
    title_ = ui::label(QString(), "Big");
    pick->addWidget(title_);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);
    grid->addWidget(ui::label(tr("Soundtrack"), "Muted"), 0, 0);
    soundtrack_ = new QComboBox;
    soundtrack_->setMinimumWidth(320);
    grid->addWidget(soundtrack_, 0, 1);
    soundtrackInfo_ = ui::label(QString(), "Muted");
    soundtrackInfo_->setWordWrap(true);
    grid->addWidget(soundtrackInfo_, 0, 2, 1, 2);
    grid->addWidget(ui::label(tr("Select screen"), "Muted"), 1, 0);
    selectRow_.choice = new QComboBox;
    selectRow_.choice->setMinimumWidth(320);
    selectRow_.plays = ui::label(QString(), "Muted");
    selectRow_.plays->setWordWrap(true);
    selectRow_.play = new QPushButton(QStringLiteral("▶"));
    selectRow_.play->setFixedWidth(36);
    selectRow_.play->setToolTip(tr("Hear it"));
    grid->addWidget(selectRow_.choice, 1, 1);
    grid->addWidget(selectRow_.plays, 1, 2);
    grid->addWidget(selectRow_.play, 1, 3);
    grid->setColumnStretch(2, 1);
    pick->addLayout(grid);
    connect(soundtrack_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        config_.soundtrack = soundtrack_->currentData().toString();
        changed();
    });
    connect(selectRow_.choice, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        config_.select = choiceOf(selectRow_.choice);
        changed();
    });
    connect(selectRow_.play, &QPushButton::clicked, this, [this] { preview(selectRow_); });

    // the stages
    QVBoxLayout* stages = nullptr;
    root->addWidget(ui::card(tr("Stages"), &stages), 1);
    auto* stageNote = ui::label(tr("Auto takes the soundtrack's best fit for the stage; or pick any song. A stage plays its "
                                   "song from the next fight on; the select screen from the next time the game starts."),
                                "Muted");
    stageNote->setWordWrap(true);
    stages->addWidget(stageNote);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    stageBox_ = new QWidget;
    stageGrid_ = new QGridLayout(stageBox_);
    stageGrid_->setHorizontalSpacing(10);
    stageGrid_->setVerticalSpacing(4);
    stageGrid_->setContentsMargins(0, 0, 8, 0);
    scroll->setWidget(stageBox_);
    stages->addWidget(scroll, 1);

    notes_ = ui::label(QString(), "Warning");
    notes_->setWordWrap(true);
    notes_->hide();
    root->addWidget(notes_);
    auto* bottom = new QHBoxLayout;
    status_ = ui::label(QString(), "Muted");
    status_->setWordWrap(true);
    apply_ = new QPushButton(tr("Apply now"));
    bottom->addWidget(status_, 1);
    bottom->addWidget(apply_);
    root->addLayout(bottom);
    connect(apply_, &QPushButton::clicked, this, &JukeboxPage::applyNow);

    applyTimer_.setSingleShot(true);
    applyTimer_.setInterval(1500);
    connect(&applyTimer_, &QTimer::timeout, this, &JukeboxPage::applyNow);
    connect(&applier_, &jukebox::Applier::progress, this, [this](int done, int total, const QString& what) {
        status_->setText(tr("Preparing %1 of %2: %3").arg(done + 1).arg(total).arg(what));
    });
    connect(&applier_, &jukebox::Applier::finished, this, [this](bool ok, const QString& message) {
        status_->setText(message);
        status_->setObjectName(ok ? QStringLiteral("Muted") : QStringLiteral("Warning"));
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
        apply_->setEnabled(true);
        applog::write(QStringLiteral("jukebox: %1: %2").arg(gameId_, message));
        if (applyAgain_) {
            applyAgain_ = false;
            applyNow();
        }
    });
    connect(&preview_, &jukebox::Preview::stateChanged, this, [this](bool playing, const QString& file) {
        auto mark = [&](const Row& r) {
            if (!r.play) return;
            const bool mine = playing && r.play->property("file").toString() == file;
            r.play->setText(mine ? QStringLiteral("■") : QStringLiteral("▶"));
        };
        mark(selectRow_);
        for (const Row& r : rows_) mark(r);
    });

    tracks_ = new QStandardItemModel(this);
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
    showConfig();
}

void JukeboxPage::rescan() {
    const bool was = loading_;
    loading_ = true;  // the combos lose their place while the model is rebuilt; showConfig puts it back
    library_ = jukebox::scanLibrary(jukebox::libraryFolder());
    folder_->setText(QDir::toNativeSeparators(jukebox::libraryFolder()));
    int tracks = 0;
    for (const auto& s : library_) tracks += int(s.tracks.size());
    if (library_.isEmpty())
        libraryInfo_->setText(tr("No music yet. Put each soundtrack in a folder of its own inside this one (for example "
                                 "\"mk2\" with its songs as m4a or mp3 files); a soundtrack.json in the folder can name the "
                                 "songs and the select-screen track, else the file names are used. Supported: %1.")
                                  .arg(jukebox::audioExtensions().join(QStringLiteral(", "))));
    else
        libraryInfo_->setText(tr("%1 soundtrack%2, %3 song%4.")
                                  .arg(library_.size()).arg(library_.size() == 1 ? QString() : QStringLiteral("s"))
                                  .arg(tracks).arg(tracks == 1 ? QString() : QStringLiteral("s")));
    rebuildTrackModel();
    loading_ = was;
}

void JukeboxPage::rebuildTrackModel() {
    tracks_->clear();
    auto add = [this](const QString& text, const QString& data, bool enabled = true) {
        auto* item = new QStandardItem(text);
        item->setData(data, Qt::UserRole);
        if (!enabled) {
            item->setEnabled(false);
            item->setSelectable(false);
        }
        tracks_->appendRow(item);
    };
    add(tr("Auto"), QStringLiteral("auto"));
    add(tr("The game's own"), QStringLiteral("own"));
    for (const auto& s : library_) {
        add(QStringLiteral("— %1 —").arg(s.title), QString(), false);
        for (const auto& t : s.tracks)
            add(QStringLiteral("   %1").arg(t.title), s.id + QLatin1Char('/') + QFileInfo(t.file).fileName());
    }
    const QSignalBlocker block(soundtrack_);
    soundtrack_->clear();
    soundtrack_->addItem(tr("The game's own music"), QString());
    for (const auto& s : library_) soundtrack_->addItem(s.title, s.id);
    selectRow_.choice->setModel(tracks_);
    for (const Row& r : rows_) r.choice->setModel(tracks_);
}

void JukeboxPage::rebuildRows() {
    for (const Row& r : rows_) {
        delete r.choice;
        delete r.plays;
        delete r.play;
    }
    rows_.clear();
    while (QLayoutItem* item = stageGrid_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const jukebox::GameMusic* gm = jukebox::gameMusic(gameId_);
    if (!gm) return;
    stageGrid_->addWidget(ui::label(tr("Stage"), "Muted"), 0, 0);
    stageGrid_->addWidget(ui::label(tr("Song"), "Muted"), 0, 1);
    stageGrid_->addWidget(ui::label(tr("Plays"), "Muted"), 0, 2);
    int line = 1;
    for (const jukebox::Stage& s : gm->stages) {
        Row r;
        r.stage = s.id;
        auto* name = new QLabel(s.name);
        r.choice = new QComboBox;
        r.choice->setModel(tracks_);
        r.choice->setMinimumWidth(300);
        r.plays = ui::label(QString(), "Muted");
        r.plays->setWordWrap(true);
        r.play = new QPushButton(QStringLiteral("▶"));
        r.play->setFixedWidth(36);
        r.play->setToolTip(tr("Hear it"));
        stageGrid_->addWidget(name, line, 0);
        stageGrid_->addWidget(r.choice, line, 1);
        stageGrid_->addWidget(r.plays, line, 2);
        stageGrid_->addWidget(r.play, line, 3);
        const int stage = s.id;
        QComboBox* box = r.choice;
        connect(box, &QComboBox::currentIndexChanged, this, [this, stage, box] {
            if (loading_) return;
            const QString c = choiceOf(box);
            if (c == QLatin1String("auto")) config_.stages.remove(stage);
            else config_.stages.insert(stage, c);
            changed();
        });
        const int index = int(rows_.size());
        connect(r.play, &QPushButton::clicked, this, [this, index] {
            if (index < rows_.size()) preview(rows_[index]);
        });
        rows_ << r;
        line++;
    }
    stageGrid_->setColumnStretch(2, 1);
    stageGrid_->setRowStretch(line, 1);
}

QString JukeboxPage::choiceOf(QComboBox* box) const {
    const QString v = box->currentData(Qt::UserRole).toString();
    return v.isEmpty() ? QStringLiteral("auto") : v;
}

void JukeboxPage::selectChoice(QComboBox* box, const QString& choice) {
    const QString want = choice.isEmpty() ? QStringLiteral("auto") : choice;
    for (int i = 0; i < tracks_->rowCount(); ++i)
        if (tracks_->item(i)->data(Qt::UserRole).toString().compare(want, Qt::CaseInsensitive) == 0) {
            box->setCurrentIndex(i);
            return;
        }
    box->setCurrentIndex(kAutoRow);
}

void JukeboxPage::setGame(const QString& gameId) {
    Page::setGame(gameId);
    const Game* g = GameData::instance().game(gameId);
    title_->setText(g ? g->title : gameId);
    rebuildRows();
    showConfig();
}

void JukeboxPage::showConfig() {
    loading_ = true;
    config_ = jukebox::loadConfig(gameId_);
    int si = soundtrack_->findData(config_.soundtrack);
    if (si < 0 && !config_.soundtrack.isEmpty()) {
        soundtrack_->addItem(tr("%1 (not in the folder)").arg(config_.soundtrack), config_.soundtrack);
        si = soundtrack_->count() - 1;
    }
    soundtrack_->setCurrentIndex(std::max(0, si));
    selectChoice(selectRow_.choice, config_.select);
    for (const Row& r : rows_) selectChoice(r.choice, config_.stages.value(r.stage, QStringLiteral("auto")));
    loading_ = false;
    refreshPlan();
}

void JukeboxPage::refreshPlan() {
    plan_ = jukebox::resolve(gameId_, config_, library_);
    const jukebox::Soundtrack* chosen = jukebox::find(library_, config_.soundtrack);
    if (chosen) {
        int stage = 0;
        for (const auto& t : chosen->tracks)
            if (t.kind == QLatin1String("stage")) stage++;
        soundtrackInfo_->setText(tr("%1 song%2, %3 for stages%4.")
                                     .arg(chosen->tracks.size()).arg(chosen->tracks.size() == 1 ? QString() : QStringLiteral("s"))
                                     .arg(stage)
                                     .arg(chosen->select >= 0 ? tr(", select screen: %1").arg(chosen->tracks[chosen->select].title)
                                                              : tr(", no select-screen track")));
    } else {
        soundtrackInfo_->setText(config_.soundtrack.isEmpty() ? tr("Each stage and the select screen keep the game's own music unless a song is picked below.")
                                                               : QString());
    }
    auto show = [&](const Row& r, const jukebox::Pick& p) {
        if (!r.plays) return;
        if (p.file.isEmpty()) {
            r.plays->setText(tr("the game's own"));
            r.play->setProperty("file", QString());
            r.play->setEnabled(false);
        } else {
            const jukebox::Soundtrack* s = jukebox::find(library_, p.soundtrack);
            r.plays->setText(QStringLiteral("%1: %2").arg(s ? s->title : p.soundtrack, p.title));
            r.play->setProperty("file", p.file);
            r.play->setEnabled(true);
        }
    };
    show(selectRow_, plan_.select);
    for (const Row& r : rows_)
        for (const jukebox::Pick& p : plan_.stages)
            if (p.stage == r.stage) show(r, p);
    notes_->setText(plan_.notes.join(QStringLiteral("\n")));
    notes_->setVisible(!plan_.notes.isEmpty());
}

void JukeboxPage::changed() {
    jukebox::saveConfig(gameId_, config_);
    refreshPlan();
    status_->setText(tr("Applying in a moment…"));
    applyTimer_.start();
}

void JukeboxPage::applyNow() {
    applyTimer_.stop();
    if (applier_.running()) {
        applyAgain_ = true;
        return;
    }
    apply_->setEnabled(false);
    status_->setText(tr("Preparing…"));
    if (!applier_.apply(gameId_, plan_)) apply_->setEnabled(true);
}

void JukeboxPage::preview(const Row& row) {
    const QString file = row.play ? row.play->property("file").toString() : QString();
    if (file.isEmpty()) return;
    if (preview_.playing() && row.play->text() == QStringLiteral("■")) {
        preview_.stop();
        return;
    }
    preview_.play(file);
}
