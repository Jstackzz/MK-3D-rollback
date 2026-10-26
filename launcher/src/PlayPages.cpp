#include "PlayPages.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QVBoxLayout>

#include "AppPaths.h"
#include "GameData.h"
#include "ServerClient.h"

namespace {
const Game* gameOf(const QString& id) { return GameData::instance().game(id); }

void fillCharacters(QComboBox* box, const QList<Character>& roster, const QString& select, int fallback = 0) {
    const QSignalBlocker block(box);
    box->clear();
    for (const auto& c : roster) box->addItem(c.name, c.id);
    const int i = box->findData(select);
    box->setCurrentIndex(i >= 0 ? i : qMin(fallback, box->count() - 1));
}
}  // namespace

// --- QueuePage -------------------------------------------------------------------------------------

QueuePage::QueuePage(bool ranked, ServerClient* server, MatchController* controller, QWidget* parent)
    : Page(parent), ranked_(ranked), server_(server), controller_(controller) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 20, 28, 16);
    root->setSpacing(14);
    root->addWidget(ui::pageHeader(
        ranked ? tr("Ranked Kombat") : tr("Kasual Kombat"),
        ranked ? tr("Sets against one opponent: the first to win 2 matches takes the set, and Elo changes once, when "
                    "it ends. Leaving a set after a match of it has counted loses it. Your opponent is revealed once "
                    "both have accepted.")
               : tr("Quick matches against the first compatible player in your region, with a rematch whenever you "
                    "both want one. Wins and losses are recorded; Elo is not touched.")));

    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    root->addLayout(columns, 1);

    QVBoxLayout* left = nullptr;
    columns->addWidget(ui::card(tr("Mode and fighter"), &left), 3);
    mode_ = new QComboBox;
    left->addWidget(mode_);
    modeInfo_ = ui::label(QString(), "Muted");
    modeInfo_->setWordWrap(true);
    left->addWidget(modeInfo_);
    pickInGame_ = new QCheckBox(tr("Pick my fighter on the game's own select screen"));
    pickInGame_->setChecked(cfg::pickInGame());
    left->addWidget(pickInGame_);
    picker_ = new CharacterPicker;
    left->addWidget(picker_, 1);
    inGamePick_ = ui::label(QString(), "Muted");
    inGamePick_->setAlignment(Qt::AlignCenter);
    inGamePick_->setWordWrap(true);
    left->addWidget(inGamePick_, 1);

    auto* rightCol = new QVBoxLayout;
    rightCol->setSpacing(16);
    columns->addLayout(rightCol, 2);
    QVBoxLayout* mm = nullptr;
    mmCard_ = ui::card(tr("Matchmaking"), &mm);
    rightCol->addWidget(mmCard_);
    // in its place while a series runs (region and ping do not matter then): the score and the next match
    QVBoxLayout* ser = nullptr;
    seriesCard_ = ui::card(QString(), &ser);
    rightCol->addWidget(seriesCard_);
    seriesTitle_ = ui::label(QString(), "CardTitle");
    ser->addWidget(seriesTitle_);
    seriesScore_ = ui::label(QString(), "Big");
    seriesScore_->setAlignment(Qt::AlignCenter);
    seriesScore_->setMinimumHeight(seriesScore_->sizeHint().height() + 4);
    ser->addWidget(seriesScore_);
    seriesInfo_ = ui::label(QString(), "Muted");
    seriesInfo_->setWordWrap(true);
    seriesInfo_->setMinimumHeight(seriesInfo_->fontMetrics().lineSpacing() + 4);
    ser->addWidget(seriesInfo_);
    auto* seriesButtons = new QHBoxLayout;
    rematch_ = new QPushButton(tr("REMATCH"));
    rematch_->setObjectName(QStringLiteral("Primary"));
    rematch_->setProperty("compact", true);
    rematch_->setMinimumHeight(36);
    rematch_->setVisible(!ranked);
    rematch_->setToolTip(tr("Fight again with the same fighters (when you both choose it)"));
    select_ = new QPushButton(tr("SELECT"));
    select_->setMinimumHeight(36);
    select_->setVisible(!ranked);
    select_->setToolTip(tr("Both go back to character select to pick again"));
    leave_ = new QPushButton(ranked ? tr("LEAVE SET") : tr("LEAVE"));
    leave_->setMinimumHeight(36);
    seriesButtons->addWidget(rematch_, 1);
    seriesButtons->addWidget(select_, 1);
    seriesButtons->addWidget(leave_, 1);
    ser->addLayout(seriesButtons);
    seriesCard_->setVisible(false);
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    region_ = new QComboBox;
    ui::fillRegions(region_, cfg::region());
    maxPing_ = new QComboBox;
    ui::fillMaxPing(maxPing_, cfg::maxPing());
    form->addRow(tr("Region"), region_);
    form->addRow(tr("Max ping"), maxPing_);
    mm->addLayout(form);
    strict_ = new QCheckBox(tr("Strict: close ratings and my region only"));
    strict_->setChecked(cfg::strict());
    strict_->setVisible(ranked);
    mm->addWidget(strict_);
    delayInfo_ = ui::label(QString(), "Muted");
    mm->addWidget(delayInfo_);

    QVBoxLayout* rec = nullptr;
    rightCol->addWidget(ui::card(QString(), &rec));
    recordTitle_ = ui::label(QString(), "CardTitle");
    rec->addWidget(recordTitle_);
    auto* tiles = new QHBoxLayout;
    if (ranked) tiles->addWidget(ui::statTile(tr("Elo"), &elo_));
    else elo_ = nullptr;
    tiles->addWidget(ui::statTile(tr("Wins"), &wins_));
    tiles->addWidget(ui::statTile(tr("Losses"), &losses_));
    rec->addLayout(tiles);

    find_ = new QPushButton(tr("FIND MATCH"));
    find_->setObjectName(QStringLiteral("Primary"));
    find_->setMinimumHeight(52);
    ui::letterSpacing(find_, 4);
    rightCol->addWidget(find_);
    status_ = ui::label(tr("Not searching."), "Muted");
    status_->setWordWrap(true);
    timer_ = ui::label(QString(), "Big");
    auto* statusRow = new QHBoxLayout;
    statusRow->addWidget(status_, 1);
    statusRow->addWidget(timer_);
    rightCol->addLayout(statusRow);
    rightCol->addStretch();

    log_ = new LogView;
    log_->setMaximumHeight(96);
    log_->setMinimumHeight(48);  // the smallest window gives the series card the room
    root->addWidget(log_);

    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        cfg::set(key(QStringLiteral("mode")), mode_->currentData());
        refreshRoster();
        updateStats();
    });
    connect(picker_, &CharacterPicker::picked, this, [this](const QString& id) {
        cfg::set(QStringLiteral("ui/%1/%2/character").arg(gameId_, mode_->currentData().toString()), id);
    });
    connect(region_, &QComboBox::currentIndexChanged, this, [this] { cfg::set(QStringLiteral("online/region"), region_->currentData()); });
    connect(maxPing_, &QComboBox::currentIndexChanged, this, [this] { cfg::set(QStringLiteral("online/max_ping"), maxPing_->currentData()); });
    connect(strict_, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("online/strict"), on); });
    connect(pickInGame_, &QCheckBox::toggled, this, [this](bool on) {
        cfg::set(QStringLiteral("online/pick_in_game"), on);
        refreshRoster();
    });
    connect(find_, &QPushButton::clicked, this, [this] {
        const auto p = controller_->phase();
        if (p == MatchController::Phase::Idle) {
            if (leavingLosesSet()) startSearch();  // a new search leaves the series (and a ranked set with it)
        } else if (controller_->lastRequest().ranked == ranked_) {
            controller_->cancelSearch();
        }
    });
    connect(rematch_, &QPushButton::clicked, this, [this] { choose(QStringLiteral("rematch")); });
    connect(select_, &QPushButton::clicked, this, [this] { choose(QStringLiteral("select")); });
    connect(leave_, &QPushButton::clicked, this, [this] {
        if (leavingLosesSet()) controller_->seriesReady(false);
    });
    connect(controller_, &MatchController::seriesChanged, this, &QueuePage::onSeries);
    seriesClock_.setInterval(1000);
    connect(&seriesClock_, &QTimer::timeout, this, &QueuePage::updateSeries);
    connect(controller_, &MatchController::phaseChanged, this, &QueuePage::onPhase);
    connect(controller_, &MatchController::logLine, this, [this](const QString& t) {
        if (controller_->lastRequest().ranked == ranked_ && controller_->phase() != MatchController::Phase::Solo &&
            controller_->phase() != MatchController::Phase::LocalTest)
            log_->line(t);
    });
    connect(controller_, &MatchController::matchFinished, this, [this](const QJsonObject& r) {
        if (r.value(QStringLiteral("ranked")).toBool() != ranked_) return;
        QString status = r.value(QStringLiteral("status")).toString();
        const QString opp = r.value(QStringLiteral("opponent")).toString();
        QString text;
        if (status == QLatin1String("agreed") && r.contains(QStringLiteral("you_won")))
            text = r.value(QStringLiteral("you_won")).toBool()
                       ? tr("You beat %1 %2 (confirmed by both players)").arg(opp, r.value(QStringLiteral("score")).toString())
                       : tr("You lost to %1 %2 (confirmed by both players)").arg(opp, r.value(QStringLiteral("score")).toString());
        else if (status == QLatin1String("disputed"))
            text = tr("The two reports differ; the match does not count.");
        else
            text = tr("Match against %1 ended: %2 (does not count)").arg(opp, status.replace(QLatin1Char('_'), QLatin1Char(' ')));
        const QJsonObject eloObj = r.value(QStringLiteral("elo")).toObject().value(server_->accountName()).toObject();
        if (!eloObj.isEmpty())
            text += tr(", Elo %1 -> %2 (%3%4)").arg(eloObj.value(QStringLiteral("before")).toInt())
                        .arg(eloObj.value(QStringLiteral("after")).toInt())
                        .arg(eloObj.value(QStringLiteral("delta")).toInt() >= 0 ? QStringLiteral("+") : QString())
                        .arg(eloObj.value(QStringLiteral("delta")).toInt());
        log_->line(text);
        server_->send({{QStringLiteral("type"), QStringLiteral("profile")}});
    });
    connect(server_, &ServerClient::loggedIn, this, [this](const QJsonObject& acc) {
        stats_ = acc.value(QStringLiteral("profile")).toObject().value(QStringLiteral("stats")).toObject();
        updateStats();
        updateButton();
    });
    connect(server_, &ServerClient::stateChanged, this, [this] { updateButton(); });
    connect(server_, &ServerClient::message, this, [this](const QJsonObject& m) {
        if (m.value(QStringLiteral("type")).toString() == QLatin1String("profile") &&
            m.value(QStringLiteral("name")).toString() == server_->accountName()) {
            stats_ = m.value(QStringLiteral("stats")).toObject();
            updateStats();
        }
    });
    clock_.setInterval(500);
    connect(&clock_, &QTimer::timeout, this, [this] {
        const int s = controller_->searchSeconds();
        timer_->setText(QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0')));
    });
    updateButton();
}

QString QueuePage::key(const QString& what) const {
    return QStringLiteral("ui/%1/%2/%3").arg(ranked_ ? QStringLiteral("ranked") : QStringLiteral("casual"), gameId_, what);
}

void QueuePage::setGame(const QString& gameId) {
    Page::setGame(gameId);
    refreshModes();
}

void QueuePage::refreshModes() {
    const Game* g = gameOf(gameId_);
    if (!g) return;
    const QSignalBlocker block(mode_);
    mode_->clear();
    for (const auto& m : g->onlineModes())
        if (!ranked_ || m.ranked) mode_->addItem(m.name, m.id);
    const int i = mode_->findData(cfg::get(key(QStringLiteral("mode"))).toString());
    mode_->setCurrentIndex(i >= 0 ? i : 0);
    refreshRoster();
    updateStats();
}

void QueuePage::refreshRoster() {
    const Game* g = gameOf(gameId_);
    if (!g) return;
    const QString modeId = mode_->currentData().toString();
    const GameMode* m = g->mode(modeId);
    modeInfo_->setText(m ? m->description : QString());
    {
        const QSignalBlocker block(pickInGame_);
        pickInGame_->setChecked(cfg::pickInGame());  // the other queue page may have changed it
    }
    const bool team = m && m->picksInGame();  // the mode's own (Chess Kombat's teams)
    const bool inGame = team || pickInGame_->isChecked();
    pickInGame_->setVisible(!team);
    picker_->setVisible(!inGame);
    inGamePick_->setVisible(inGame);
    inGamePick_->setText(team ? tr("Teams are picked inside the game for this mode.")
                              : tr("You pick your fighter on the game's select screen when the match starts, with "
                                   "your own controller; your opponent picks on theirs."));
    picker_->setRoster(g->roster(modeId), cfg::get(QStringLiteral("ui/%1/%2/character").arg(gameId_, modeId)).toString());
    delayInfo_->setText(tr("Input delay %1 frame(s), game UDP port %2 (change in Settings)")
                            .arg(qMax(ranked_ ? 1 : 0, cfg::inputDelay())).arg(cfg::gamePort()));
}

void QueuePage::updateStats() {
    const Game* g = gameOf(gameId_);
    const QString modeId = mode_->currentData().toString();
    recordTitle_->setText(tr("YOUR RECORD  |  %1 %2").arg(g ? g->shortName.toUpper() : QString(), mode_->currentText().toUpper()));
    const QJsonObject entry = stats_.value(gameId_).toObject().value(modeId).toObject()
                                  .value(ranked_ ? QStringLiteral("ranked") : QStringLiteral("casual")).toObject();
    const bool online = server_->isLoggedIn();
    if (elo_) elo_->setText(online ? QString::number(entry.value(QStringLiteral("elo")).toInt(1500)) : QStringLiteral("-"));
    wins_->setText(online ? QString::number(entry.value(QStringLiteral("wins")).toInt()) : QStringLiteral("-"));
    losses_->setText(online ? QString::number(entry.value(QStringLiteral("losses")).toInt()) : QStringLiteral("-"));
}

bool QueuePage::startSearch(const QString& mode, const QString& character) {
    if (!mode.isEmpty()) {
        const int i = mode_->findData(mode);
        if (i >= 0) mode_->setCurrentIndex(i);
    }
    if (!character.isEmpty()) picker_->selectId(character);
    const Game* g = gameOf(gameId_);
    const GameMode* m = g ? g->mode(mode_->currentData().toString()) : nullptr;
    if (!g || !m) return false;
    QueueRequest req;
    req.game = gameId_;
    req.mode = m->id;
    req.ranked = ranked_;
    req.region = region_->currentData().toString();
    req.maxPing = maxPing_->currentData().toInt();
    req.strict = ranked_ && strict_->isChecked();
    req.character = m->picksInGame() || cfg::pickInGame() ? QString() : picker_->currentId();
    req.delay = qMax(ranked_ ? 1 : 0, cfg::inputDelay());
    QString error;
    if (!controller_->findMatch(req, &error)) {
        log_->line(error);
        QMessageBox::warning(this, tr("Cannot search"), error);
        return false;
    }
    log_->line(tr("Searching: %1 %2 as %3 in %4, max ping %5").arg(g->shortName, m->name,
               m->picksInGame() ? tr("in-game team")
                                : req.character.isEmpty() ? tr("a fighter picked in the game") : picker_->currentName(),
               GameData::regionName(req.region), maxPing_->currentText()));
    return true;
}

void QueuePage::onPhase(MatchController::Phase, const QString& text) {
    if (controller_->lastRequest().ranked == ranked_) status_->setText(text);
    updateButton();
    updateSeries();
}

void QueuePage::onSeries(const QJsonObject& s) {
    if (s.value(QStringLiteral("ranked")).toBool() != ranked_) return;  // the other queue's
    if (s.value(QStringLiteral("state")).toString() == QLatin1String("over")) {
        log_->line(seriesOverText(s));
        series_ = {};
        chosen_.clear();
        seriesClock_.stop();
        seriesCard_->setVisible(false);
        mmCard_->setVisible(true);
        server_->send({{QStringLiteral("type"), QStringLiteral("profile")}});  // the record, a set's Elo
        return;
    }
    if (s.value(QStringLiteral("state")).toString() != QLatin1String("between") ||
        s.value(QStringLiteral("series")) != series_.value(QStringLiteral("series")))
        chosen_.clear();
    series_ = s;
    seriesSince_.start();
    mmCard_->setVisible(false);
    seriesCard_->setVisible(true);
    seriesClock_.start();
    updateSeries();
}

void QueuePage::choose(const QString& choice) {
    chosen_ = choice;
    controller_->seriesReady(true, choice);
    updateSeries();
}

void QueuePage::updateSeries() {
    if (series_.isEmpty()) return;
    const QString opp = series_.value(QStringLiteral("opponent")).toString();
    const bool between = series_.value(QStringLiteral("state")).toString() == QLatin1String("between");
    const bool youReady = series_.value(QStringLiteral("you_ready")).toBool() || !chosen_.isEmpty();
    const bool themReady = series_.value(QStringLiteral("them_ready")).toBool();
    const QString yours = chosen_.isEmpty() ? series_.value(QStringLiteral("you_choice")).toString() : chosen_;
    const bool themSelect = series_.value(QStringLiteral("them_choice")).toString() == QLatin1String("select");
    const bool select = yours == QLatin1String("select") || themSelect;  // either one: both pick again
    // modes whose teams or karts are always picked in the game have nothing to pick again
    const Game* g = gameOf(series_.value(QStringLiteral("game")).toString());
    const GameMode* m = g ? g->mode(series_.value(QStringLiteral("mode")).toString()) : nullptr;
    const bool canSelect = !ranked_ && !(m && m->picksInGame());
    const int left = qMax(0, series_.value(QStringLiteral("seconds_left")).toInt() -
                                 static_cast<int>(seriesSince_.elapsed() / 1000));
    seriesTitle_->setText(ranked_ ? tr("SET  |  FIRST TO %1  |  VS %2")
                                        .arg(series_.value(QStringLiteral("need")).toInt()).arg(opp.toUpper())
                                  : tr("KASUAL  |  VS %1").arg(opp.toUpper()));
    seriesScore_->setText(tr("YOU %1 - %2 %3").arg(series_.value(QStringLiteral("you")).toInt())
                              .arg(series_.value(QStringLiteral("them")).toInt()).arg(opp.toUpper()));
    QString info;
    if (!between)
        info = tr("Match %1 against %2 under way.").arg(series_.value(QStringLiteral("played")).toInt() + 1).arg(opp);
    else if (ranked_)
        info = tr("The next match starts as soon as both games are ready (%1 s). Leaving the set now counts as "
                  "losing it.").arg(left);
    else if (youReady && themReady)
        info = select ? tr("Starting: character select...") : tr("Starting the rematch...");
    else if (youReady)  // one line at the smallest window (the buttons' tooltips say what each does)
        info = (yours == QLatin1String("select") ? tr("Select chosen: waiting for %1 (%2 s).")
                                                 : tr("Rematch chosen: waiting for %1 (%2 s)."))
                   .arg(opp).arg(left);
    else if (themReady)
        info = themSelect ? tr("%1 chose SELECT! (%2 s)").arg(opp).arg(left)
                          : tr("%1 wants a rematch! (%2 s)").arg(opp).arg(left);
    else if (canSelect)
        info = tr("Same fighters, or pick again? (%1 s)").arg(left);
    else
        info = tr("Rematch? (%1 s)").arg(left);
    seriesInfo_->setText(info);
    select_->setVisible(canSelect);
    rematch_->setEnabled(between && !youReady);
    select_->setEnabled(between && !youReady);
    leave_->setEnabled(between);
}

QString QueuePage::seriesOverText(const QJsonObject& s) const {
    const QString me = server_->accountName();
    const QString opp = s.value(QStringLiteral("opponent")).toString();
    const QString outcome = s.value(QStringLiteral("outcome")).toString();
    const QString score = QStringLiteral("%1-%2").arg(s.value(QStringLiteral("you")).toInt())
                              .arg(s.value(QStringLiteral("them")).toInt());
    const bool iLeft = s.value(QStringLiteral("left")).toString() == me;
    QString text;
    if (!ranked_) {
        if (outcome == QLatin1String("left"))
            text = iLeft ? tr("You left the series against %1 (%2).").arg(opp, score)
                         : tr("%1 left the series (%2).").arg(opp, score);
        else
            text = tr("No rematch with %1 (%2).").arg(opp, score);
    } else if (outcome == QLatin1String("won")) {
        text = s.value(QStringLiteral("winner")).toString() == me ? tr("You won the set against %1, %2!").arg(opp, score)
                                                                   : tr("%1 won the set, %2.").arg(opp, score);
    } else if (outcome == QLatin1String("forfeit")) {
        text = iLeft ? tr("You left the set against %1 at %2: it counts as a loss.").arg(opp, score)
                     : tr("%1 left the set at %2: you win it.").arg(opp, score);
    } else {
        text = tr("The set against %1 ended without a result (%2): no Elo change.").arg(opp, score);
    }
    const QJsonObject e = s.value(QStringLiteral("elo")).toObject().value(me).toObject();
    if (!e.isEmpty()) {
        const int delta = e.value(QStringLiteral("delta")).toInt();
        text += tr(" Elo %1 -> %2 (%3%4).").arg(e.value(QStringLiteral("before")).toInt())
                    .arg(e.value(QStringLiteral("after")).toInt())
                    .arg(delta >= 0 ? QStringLiteral("+") : QString()).arg(delta);
    }
    return text;
}

bool QueuePage::leavingLosesSet() {
    const QJsonObject s = controller_->series();
    if (s.isEmpty() || !s.value(QStringLiteral("ranked")).toBool() ||
        s.value(QStringLiteral("you")).toInt() + s.value(QStringLiteral("them")).toInt() == 0)
        return true;
    return QMessageBox::question(this, tr("Leave the set?"),
                                 tr("You are in a ranked set against %1 (%2-%3, first to %4). Leaving it now counts "
                                    "as losing the set, with its Elo. Leave anyway?")
                                     .arg(s.value(QStringLiteral("opponent")).toString())
                                     .arg(s.value(QStringLiteral("you")).toInt())
                                     .arg(s.value(QStringLiteral("them")).toInt())
                                     .arg(s.value(QStringLiteral("need")).toInt())) == QMessageBox::Yes;
}

void QueuePage::updateButton() {
    using P = MatchController::Phase;
    const P p = controller_->phase();
    const bool mine = controller_->lastRequest().ranked == ranked_;
    const bool active = p == P::Searching || p == P::Found || p == P::Connecting;
    find_->setProperty("searching", active && mine);
    if (p == P::Idle) {
        find_->setText(tr("FIND MATCH"));
        find_->setEnabled(server_->isLoggedIn());
        const QString signIn = tr("Sign in to search for matches.");
        if (!server_->isLoggedIn()) status_->setText(signIn);
        else if (status_->text() == signIn) status_->clear();  // signed in since
        clock_.stop();
        timer_->clear();
    } else if (active && mine) {
        find_->setText(tr("CANCEL SEARCH"));
        find_->setEnabled(true);
        if (!clock_.isActive()) clock_.start();
    } else {
        find_->setText(p == P::Playing || p == P::Starting || p == P::Finishing ? tr("IN MATCH") : tr("BUSY"));
        find_->setEnabled(false);
        clock_.stop();
        timer_->clear();
    }
    find_->style()->unpolish(find_);
    find_->style()->polish(find_);
}

// --- SinglePlayerPage ------------------------------------------------------------------------------

SinglePlayerPage::SinglePlayerPage(MatchController* controller, QWidget* parent) : Page(parent), controller_(controller) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 20, 28, 16);
    root->setSpacing(14);
    root->addWidget(ui::pageHeader(tr("Single player"),
                                   tr("The whole game on this PC with its own menus, as on the console: arcade, "
                                      "practice, its other modes. What you unlock and set is saved in the game's "
                                      "folder.")));
    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    root->addLayout(columns);

    QVBoxLayout* solo = nullptr;
    columns->addWidget(ui::card(tr("Play"), &solo), 1);
    title_ = ui::label(QString(), "Big");
    title_->setWordWrap(true);
    solo->addWidget(title_);
    auto* row = new QHBoxLayout;
    play_ = new QPushButton(tr("PLAY"));
    play_->setObjectName(QStringLiteral("Primary"));
    close_ = new QPushButton(tr("Close the game"));
    row->addWidget(play_, 1);
    row->addWidget(close_);
    solo->addLayout(row);
    exeInfo_ = ui::label(QString(), "Muted");
    exeInfo_->setWordWrap(true);
    solo->addWidget(exeInfo_);
    auto* onlineNote = ui::label(tr("Online matches wait while it is open: close the game to search for one."), "Muted");
    onlineNote->setWordWrap(true);
    solo->addWidget(onlineNote);
    solo->addStretch();

    QVBoxLayout* dev = nullptr;
    QFrame* devCard = ui::card(tr("Local test (development)"), &dev);
    devCard->setVisible(!paths::isPackage());  // a player package has no use for it
    columns->addWidget(devCard, 1);
    auto* devInfo = ui::label(tr("Starts two game instances on this PC connected over loopback "
                                 "(UDP %1 and %2) with matching start lines. No server involved.")
                                  .arg(cfg::localTestPort(1)).arg(cfg::localTestPort(2)),
                              "Muted");
    devInfo->setWordWrap(true);
    dev->addWidget(devInfo);
    auto* devForm = new QFormLayout;
    testMode_ = new QComboBox;
    testP1_ = new QComboBox;
    testP2_ = new QComboBox;
    devForm->addRow(tr("Mode"), testMode_);
    devForm->addRow(tr("Player 1"), testP1_);
    devForm->addRow(tr("Player 2"), testP2_);
    dev->addLayout(devForm);
    auto* devRow = new QHBoxLayout;
    auto* test = new QPushButton(tr("Local test"));
    auto* testStop = new QPushButton(tr("Stop both"));
    devRow->addWidget(test, 1);
    devRow->addWidget(testStop);
    dev->addLayout(devRow);
    dev->addStretch();

    status_ = ui::label(tr("Idle."), "Muted");
    root->addWidget(status_);
    log_ = new LogView;
    root->addWidget(log_, 1);

    connect(play_, &QPushButton::clicked, this, [this] {
        QString err;
        if (!controller_->startSolo(gameId_, &err)) {
            log_->line(err);
            QMessageBox::warning(this, tr("Single player"), err);
        }
    });
    connect(close_, &QPushButton::clicked, controller_, &MatchController::stopSolo);
    connect(test, &QPushButton::clicked, this, [this] {
        QString err;
        if (!controller_->startLocalTest(gameId_, testMode_->currentData().toString(), testP1_->currentData().toString(),
                                         testP2_->currentData().toString(), &err)) {
            log_->line(err);
            QMessageBox::warning(this, tr("Local test"), err);
        }
    });
    connect(testStop, &QPushButton::clicked, controller_, &MatchController::stopLocalTest);
    connect(testMode_, &QComboBox::currentIndexChanged, this, &SinglePlayerPage::refreshTestRoster);
    connect(controller_, &MatchController::logLine, this, [this](const QString& t) {
        const auto p = controller_->phase();
        if (p == MatchController::Phase::Solo || p == MatchController::Phase::LocalTest ||
            t.startsWith(QLatin1String("Local test")) || t.startsWith(QLatin1String("Single player")))
            log_->line(t);
    });
    connect(controller_, &MatchController::gameStatusChanged, this, [this](const QString& s) {
        status_->setText(tr("Game: %1").arg(s));
    });
    connect(controller_, &MatchController::phaseChanged, this, &SinglePlayerPage::updateButtons);
    updateButtons();
}

void SinglePlayerPage::updateButtons() {
    const bool open = controller_->soloRunning();
    const bool idle = controller_->phase() == MatchController::Phase::Idle;
    play_->setText(open ? tr("PLAYING") : tr("PLAY"));
    play_->setEnabled(!open && idle);
    play_->setToolTip(!open && !idle ? tr("Busy: %1").arg(controller_->phaseText()) : QString());
    close_->setEnabled(open);
}

void SinglePlayerPage::setGame(const QString& gameId) {
    Page::setGame(gameId);
    const Game* g = gameOf(gameId);
    if (!g) return;
    title_->setText(g->title);
    {
        const QSignalBlocker block(testMode_);
        testMode_->clear();
        for (const auto& m : g->onlineModes()) testMode_->addItem(m.name, m.id);
    }
    refreshTestRoster();
    const QString exe = cfg::gameExe(gameId);
    exeInfo_->setText(QFileInfo::exists(exe) ? tr("Game: %1").arg(QDir::toNativeSeparators(exe))
                                             : tr("Game not built yet: %1 (see Build game)").arg(QDir::toNativeSeparators(exe)));
}

void SinglePlayerPage::refreshTestRoster() {
    const Game* g = gameOf(gameId_);
    if (!g) return;
    const QList<Character> roster = g->roster(testMode_->currentData().toString());
    fillCharacters(testP1_, roster, QStringLiteral("Scorpion"));
    fillCharacters(testP2_, roster, QStringLiteral("SubZero"));
}

// --- LeaderboardPage -------------------------------------------------------------------------------

LeaderboardPage::LeaderboardPage(ServerClient* server, QWidget* parent) : Page(parent), server_(server) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 20, 28, 16);
    root->setSpacing(12);
    root->addWidget(ui::pageHeader(tr("Leaderboard"), tr("Top ranked players per game and mode, by Elo.")));
    auto* row = new QHBoxLayout;
    mode_ = new QComboBox;
    mode_->setMinimumWidth(200);
    auto* refreshBtn = new QPushButton(tr("Refresh"));
    me_ = ui::label(QString(), "Big");
    row->addWidget(mode_);
    row->addWidget(refreshBtn);
    row->addStretch();
    row->addWidget(me_);
    root->addLayout(row);
    table_ = new QTableWidget(0, 6);
    table_->setHorizontalHeaderLabels({tr("Rank"), tr("Player"), tr("Elo"), tr("Wins"), tr("Losses"), tr("Win %")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAlternatingRowColors(true);
    root->addWidget(table_, 1);
    info_ = ui::label(QString(), "Muted");
    root->addWidget(info_);
    connect(refreshBtn, &QPushButton::clicked, this, &LeaderboardPage::refresh);
    connect(mode_, &QComboBox::currentIndexChanged, this, &LeaderboardPage::refresh);
    connect(server_, &ServerClient::message, this, &LeaderboardPage::onMessage);
    connect(server_, &ServerClient::stateChanged, this, [this](ServerClient::State s) {
        if (s == ServerClient::State::Online || s == ServerClient::State::LoggedIn) refresh();
    });
}

void LeaderboardPage::setGame(const QString& gameId) {
    Page::setGame(gameId);
    const Game* g = gameOf(gameId);
    if (!g) return;
    {
        const QSignalBlocker block(mode_);
        mode_->clear();
        for (const auto& m : g->onlineModes())
            if (m.ranked) mode_->addItem(m.name, m.id);
    }
    refresh();
}

void LeaderboardPage::refresh() {
    if (!server_->isOnline()) {
        info_->setText(tr("Offline."));
        return;
    }
    server_->send({{QStringLiteral("type"), QStringLiteral("leaderboard")}, {QStringLiteral("game"), gameId_},
                   {QStringLiteral("mode"), mode_->currentData().toString()}, {QStringLiteral("limit"), 100}});
}

void LeaderboardPage::onMessage(const QJsonObject& msg) {
    if (msg.value(QStringLiteral("type")).toString() != QLatin1String("leaderboard") ||
        msg.value(QStringLiteral("game")).toString() != gameId_ ||
        msg.value(QStringLiteral("mode")).toString() != mode_->currentData().toString())
        return;
    const QJsonArray rows = msg.value(QStringLiteral("rows")).toArray();
    table_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        const QJsonObject r = rows.at(i).toObject();
        const int w = r.value(QStringLiteral("wins")).toInt(), l = r.value(QStringLiteral("losses")).toInt();
        const QStringList cells = {QString::number(r.value(QStringLiteral("rank")).toInt()),
                                   r.value(QStringLiteral("name")).toString(),
                                   QString::number(r.value(QStringLiteral("elo")).toInt()),
                                   QString::number(w),
                                   QString::number(l),
                                   w + l ? QStringLiteral("%1%").arg(100 * w / (w + l)) : QStringLiteral("-")};
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells[c]);
            item->setTextAlignment(c == 1 ? (Qt::AlignLeft | Qt::AlignVCenter) : Qt::AlignCenter);
            if (r.value(QStringLiteral("name")).toString() == server_->accountName()) item->setForeground(QColor(0xff, 0x5a, 0x66));
            table_->setItem(i, c, item);
        }
    }
    const QJsonObject me = msg.value(QStringLiteral("me")).toObject();
    me_->setText(me.isEmpty() ? QString() : tr("You: #%1  |  %2").arg(me.value(QStringLiteral("rank")).toInt())
                                                     .arg(me.value(QStringLiteral("elo")).toInt()));
    info_->setText(tr("%1 ranked players").arg(msg.value(QStringLiteral("total")).toInt()));
}
