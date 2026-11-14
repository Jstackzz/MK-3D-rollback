#include "LobbyPage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "AppPaths.h"
#include "Dialogs.h"
#include "GameData.h"
#include "MatchController.h"
#include "ServerClient.h"
#include "Style.h"

namespace {
QString s(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }

constexpr int kBadgeHeight = 24;  // a member's rank badge by their name
enum MemberColumn { kPlayer, kFighter, kStatus, kConnection, kRespect };

QTableWidget* makeTable(const QStringList& headers) {
    auto* t = new QTableWidget(0, static_cast<int>(headers.size()));
    t->setHorizontalHeaderLabels(headers);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    t->verticalHeader()->hide();
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setAlternatingRowColors(true);
    return t;
}

qint64 secondsLeft(qint64 endsMs, qint64 now) { return std::max<qint64>(0, (endsMs - now + 999) / 1000); }
}  // namespace

LobbyPage::LobbyPage(ServerClient* server, MatchController* controller, QWidget* parent)
    : Page(parent), server_(server), controller_(controller) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 20, 28, 16);
    root->setSpacing(14);
    root->addWidget(ui::pageHeader(tr("Lobbies"), tr("Private or public rooms for two players and their spectators: "
                                                     "pick fighters, chat, ready up, and the match starts; spectators "
                                                     "watch each match in their own game. King of the Hill: the winner "
                                                     "stays on, everyone else waits in line and rates each win. Lobby "
                                                     "matches count as Kasual.")));
    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    root->addLayout(columns, 1);

    QVBoxLayout* left = nullptr;
    columns->addWidget(ui::card(tr("Open lobbies"), &left), 1);
    auto* bar = new QHBoxLayout;
    auto* refreshBtn = new QPushButton(tr("Refresh"));
    auto* create = new QPushButton(tr("Create lobby"));
    code_ = new QLineEdit;
    code_->setPlaceholderText(tr("code"));
    code_->setMaxLength(5);
    code_->setMaximumWidth(90);
    auto* joinCode = new QPushButton(tr("Join code"));
    bar->addWidget(refreshBtn);
    bar->addWidget(create);
    bar->addStretch();
    bar->addWidget(code_);
    bar->addWidget(joinCode);
    left->addLayout(bar);
    list_ = makeTable({tr("Code"), tr("Name"), tr("Host"), tr("Mode"), tr("Region"), tr("Players"), tr("Lock")});
    list_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    list_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    left->addWidget(list_, 1);
    auto* joinRow = new QHBoxLayout;
    listInfo_ = ui::label(QString(), "Muted");
    listInfo_->setWordWrap(true);
    auto* joinSel = new QPushButton(tr("Join selected"));
    joinSel->setToolTip(tr("Join to play. In a King of the Hill lobby whose two places are taken you wait in line, "
                           "watching, for your turn to play the king."));
    auto* watchSel = new QPushButton(tr("Watch selected"));
    watchSel->setToolTip(tr("Join as a spectator: each of the lobby's matches plays in your game, as its players play "
                            "it. A match under way is caught up from its start."));
    joinRow->addWidget(listInfo_, 1);
    joinRow->addWidget(watchSel);
    joinRow->addWidget(joinSel);
    left->addLayout(joinRow);

    QVBoxLayout* rightBody = nullptr;
    columns->addWidget(ui::card(tr("Your lobby"), &rightBody), 1);
    right_ = new QStackedWidget;
    rightBody->addWidget(right_, 1);
    auto* none = ui::label(tr("You are not in a lobby.\nCreate one, or join one from the list or by its code."), "Muted");
    none->setAlignment(Qt::AlignCenter);
    right_->addWidget(none);

    auto* panel = new QWidget;
    auto* pv = new QVBoxLayout(panel);
    pv->setContentsMargins(0, 0, 0, 0);
    lobbyTitle_ = ui::label(QString(), "Big");
    lobbyTitle_->setTextFormat(Qt::PlainText);  // the lobby's name is whatever its host typed: never markup
    lobbyInfo_ = ui::label(QString(), "Muted");
    lobbyInfo_->setWordWrap(true);
    pv->addWidget(lobbyTitle_);
    pv->addWidget(lobbyInfo_);
    deathTraps_ = new QCheckBox(tr("Death traps"));
    pv->addWidget(deathTraps_);
    members_ = makeTable({tr("Player"), tr("Fighter"), tr("Status"), tr("Connection"), tr("Respect")});
    members_->verticalHeader()->setDefaultSectionSize(kBadgeHeight + 8);  // room for the rank badges
    // the player's badge and name take what the other columns leave
    for (int c = 1; c < members_->columnCount(); ++c)
        members_->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    if (QTableWidgetItem* head = members_->horizontalHeaderItem(kRespect))
        head->setToolTip(tr("Respect Points in this game and mode: what spectators gave each King of the Hill win, "
                            "1 to 10 each"));
    members_->setMaximumHeight(170);
    pv->addWidget(members_);
    auto* controls = new QHBoxLayout;
    character_ = new QComboBox;
    character_->setMinimumWidth(150);
    ready_ = new QPushButton(tr("Ready"));
    ready_->setCheckable(true);
    // no START: both players ready starts the match after a 3-2-1 countdown (the server's), shown here
    countdown_ = ui::label(QString(), "Big");
    countdown_->setAlignment(Qt::AlignCenter);
    nextInfo_ = ui::label(QString(), "Muted");
    nextInfo_->setAlignment(Qt::AlignCenter);
    nextInfo_->setWordWrap(true);
    tick_ = new QTimer(this);
    tick_->setInterval(100);
    leave_ = new QPushButton(tr("Leave"));
    role_ = new QPushButton(tr("Watch"));
    controls->addWidget(character_, 1);
    controls->addWidget(ready_);
    controls->addWidget(role_);
    controls->addWidget(leave_);
    pv->addLayout(controls);
    pv->addWidget(countdown_);
    pv->addWidget(nextInfo_);
    // King of the Hill: a spectator of the last match rates its winner, a whole number from 1 to 10 (their Respect
    // Points); on a controller, left and right pick, A gives it
    // (one row, so the members' table keeps its room while it shows)
    ratePanel_ = new QWidget;
    auto* rateRow = new QHBoxLayout(ratePanel_);
    rateRow->setContentsMargins(0, 2, 0, 2);
    rateRow->setSpacing(3);
    rateLabel_ = ui::label(QString(), "Rank");
    rateLabel_->setTextFormat(Qt::PlainText);
    rateLabel_->setWordWrap(true);
    rateRow->addWidget(rateLabel_, 1);
    for (int i = 1; i <= 10; ++i) {
        auto* b = new QPushButton(QString::number(i));
        b->setMinimumWidth(34);
        b->setMaximumWidth(44);
        b->setStyleSheet(QStringLiteral("QPushButton { padding: 7px 2px; }"));  // room for "10" in a narrow button
        b->setToolTip(i == 1 ? tr("Give the winner 1 Respect Point") : tr("Give the winner %1 Respect Points").arg(i));
        connect(b, &QPushButton::clicked, this, [this, i] { rate(i); });
        rateRow->addWidget(b);
        rateButtons_ << b;
    }
    ratePanel_->hide();
    pv->addWidget(ratePanel_);
    chat_ = new LogView;
    pv->addWidget(chat_, 1);
    auto* chatRow = new QHBoxLayout;
    chatInput_ = new QLineEdit;
    chatInput_->setMaxLength(200);
    chatInput_->setPlaceholderText(tr("Say something..."));
    auto* send = new QPushButton(tr("Send"));
    chatRow->addWidget(chatInput_, 1);
    chatRow->addWidget(send);
    pv->addLayout(chatRow);
    right_->addWidget(panel);

    connect(refreshBtn, &QPushButton::clicked, this, &LobbyPage::refresh);
    connect(create, &QPushButton::clicked, this, &LobbyPage::createLobby);
    connect(joinCode, &QPushButton::clicked, this, [this] { join(code_->text().trimmed().toUpper(), false); });
    connect(joinSel, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0) return;
        join(list_->item(row, 0)->text(), !list_->item(row, 6)->text().isEmpty());
    });
    connect(watchSel, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0) return;
        join(list_->item(row, 0)->text(), !list_->item(row, 6)->text().isEmpty(), true);
    });
    connect(list_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        // a lobby whose two places are taken: watch it (King of the Hill: wait in line to play)
        const bool full = list_->item(row, 5)->data(Qt::UserRole).toBool();
        join(list_->item(row, 0)->text(), !list_->item(row, 6)->text().isEmpty(), full);
    });
    connect(leave_, &QPushButton::clicked, this, [this] {
        controller_->stopWatching();
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_leave")}});
    });
    connect(role_, &QPushButton::clicked, this, [this] {
        // to play in a plain lobby the game is needed for it; King of the Hill's line keeps watching until its turn
        if (!koth() && roleAction_ == QLatin1String("player")) controller_->stopWatching();
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_role")}, {QStringLiteral("role"), roleAction_}});
    });
    connect(tick_, &QTimer::timeout, this, &LobbyPage::updateStatus);
    connect(deathTraps_, &QCheckBox::toggled, this, [this](bool on) {
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_settings")}, {QStringLiteral("death_traps"), on}});
    });
    connect(ready_, &QPushButton::clicked, this, [this](bool on) {
        if (on) {
            QString err;
            if (!controller_->prepareGame(s(lobby_, "game"), &err)) {
                ready_->setChecked(false);
                QMessageBox::warning(this, tr("Ready"), err);
                return;
            }
        }
        QJsonObject ready{{QStringLiteral("type"), QStringLiteral("lobby_ready")}, {QStringLiteral("ready"), on},
                          {QStringLiteral("delay"), cfg::inputDelay()}, {QStringLiteral("relay"), cfg::hideIp()},
                          {QStringLiteral("hide_always"), cfg::hideIpAlways()},
                          {QStringLiteral("link"), server_->linkType()}};
        if (controller_->serverRttMs() >= 0) ready.insert(QStringLiteral("server_rtt"), controller_->serverRttMs());
        server_->send(ready);
    });
    connect(character_, &QComboBox::activated, this, [this] {
        const QString id = character_->currentData().toString();  // empty: picked on the game's select screen
        if (id.isEmpty()) cfg::set(QStringLiteral("online/pick_in_game"), true);
        else {
            cfg::set(QStringLiteral("online/pick_in_game"), false);
            cfg::set(QStringLiteral("ui/%1/%2/character").arg(s(lobby_, "game"), s(lobby_, "mode")), id);
        }
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_character")}, {QStringLiteral("character"), id}});
    });
    connect(send, &QPushButton::clicked, this, &LobbyPage::sendChat);
    connect(chatInput_, &QLineEdit::returnPressed, this, &LobbyPage::sendChat);
    connect(server_, &ServerClient::message, this, &LobbyPage::onMessage);
    // what this launcher's game is doing decides whether Ready can be pressed: a game still playing (or showing) the
    // last match's end cannot take the next one yet (King of the Hill: a spectator called up to play)
    connect(controller_, &MatchController::phaseChanged, this, [this] {
        if (!inLobby()) return;
        updateControls();
        updateStatus();
    });
    // signed out or offline: the server took this player out of the lobby (a lobby is the session's), so the page
    // goes back to the list rather than showing a room it is no longer in. A dropped connection gets the place back
    // at the next login within the server's 60 s, so a spectator's game keeps watching meanwhile: from what it has,
    // then from the feed sent again
    rejoinGrace_ = new QTimer(this);
    rejoinGrace_->setSingleShot(true);
    rejoinGrace_->setInterval(70000);
    connect(rejoinGrace_, &QTimer::timeout, this, [this] {
        if (!inLobby()) controller_->stopWatching();
    });
    connect(server_, &ServerClient::stateChanged, this, [this](ServerClient::State st) {
        if (st == ServerClient::State::LoggedIn) {
            refresh();
        } else if (inLobby()) {
            const bool dropped = st == ServerClient::State::Offline && watchingHere_;
            leftLobby(dropped);
            if (dropped) rejoinGrace_->start();
        }
    });
}

void LobbyPage::pressReady() {
    if (inLobby() && ready_->isVisible() && ready_->isEnabled()) ready_->click();
}

void LobbyPage::focusReady() {
    if (!inLobby()) return;
    if (!ratePanel_->isHidden()) {  // the rating has a time; Ready waits
        rateButtons_[4]->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (ready_->isVisible() && ready_->isEnabled()) ready_->setFocus(Qt::OtherFocusReason);
}

bool LobbyPage::lobbyOnly() const {
    if (!inLobby()) return false;
    const Game* g = GameData::instance().game(s(lobby_, "game"));
    const GameMode* m = g ? g->mode(s(lobby_, "mode")) : nullptr;
    return m && m->lobbyOnly;
}

void LobbyPage::setGame(const QString& gameId) {
    Page::setGame(gameId);
    refresh();
}

void LobbyPage::showEvent(QShowEvent* event) {
    Page::showEvent(event);
    refresh();
}

void LobbyPage::refresh() {
    if (!server_->isLoggedIn()) {
        listInfo_->setText(tr("Sign in to see lobbies."));
        list_->setRowCount(0);
        return;
    }
    server_->send({{QStringLiteral("type"), QStringLiteral("lobby_list")}, {QStringLiteral("game"), gameId_}});
}

QString LobbyPage::savedCharacter(const QString& game, const QString& mode) const {
    const Game* g = GameData::instance().game(game);
    const GameMode* m = g ? g->mode(mode) : nullptr;
    // the game's own select screen; Chess Kombat always: each player builds their own team there (the game built both
    // teams around the leader the lobby sent until 1.0.8, the queue's players their own)
    if ((m && m->picksInGame()) || cfg::pickInGame()) return QString();
    QString id = cfg::get(QStringLiteral("ui/%1/%2/character").arg(game, mode)).toString();
    if (id.isEmpty() && g) {
        const auto roster = g->roster(mode);
        if (!roster.isEmpty()) id = roster.first().id;
    }
    return id;
}

void LobbyPage::createLobby() {
    const Game* g = GameData::instance().game(gameId_);
    if (!g || !server_->isLoggedIn()) {
        QMessageBox::information(this, tr("Lobby"), tr("Sign in first."));
        return;
    }
    CreateLobbyDialog dlg(*g, tr("%1's lobby").arg(server_->accountName()), server_->serverHas(QStringLiteral("koth")),
                          this);
    if (dlg.exec() != QDialog::Accepted) return;
    QJsonObject msg{{QStringLiteral("type"), QStringLiteral("lobby_create")},
                    {QStringLiteral("name"), dlg.name()},
                    {QStringLiteral("game"), gameId_},
                    {QStringLiteral("mode"), dlg.mode()},
                    {QStringLiteral("password"), dlg.password()},
                    {QStringLiteral("rounds"), dlg.rounds()},
                    {QStringLiteral("region"), cfg::region()},
                    {QStringLiteral("character"), savedCharacter(gameId_, dlg.mode())},
                    {QStringLiteral("death_traps"), dlg.deathTraps()},  // the window's box (0.1.7 never sent it)
                    {QStringLiteral("link"), server_->linkType()}};
    if (dlg.kingOfTheHill()) msg.insert(QStringLiteral("koth"), true);
    if (controller_->serverRttMs() >= 0) msg.insert(QStringLiteral("server_rtt"), controller_->serverRttMs());
    server_->send(msg);
}

QJsonObject LobbyPage::me() const {
    for (const auto& mv : lobby_.value(QStringLiteral("members")).toArray()) {
        const QJsonObject mem = mv.toObject();
        if (s(mem, "name") == server_->accountName()) return mem;
    }
    return {};
}

bool LobbyPage::spectating() const { return me().value(QStringLiteral("spectator")).toBool(); }

bool LobbyPage::koth() const { return lobby_.value(QStringLiteral("koth")).toBool(); }

void LobbyPage::join(const QString& code, bool locked, bool spectate) {
    if (code.size() != 5) {
        listInfo_->setText(tr("Lobby codes have 5 characters."));
        return;
    }
    QString password;
    if (locked) {
        bool ok = false;
        password = QInputDialog::getText(this, tr("Lobby %1").arg(code), tr("Password"), QLineEdit::Password, QString(), &ok);
        if (!ok) return;
    }
    QString game = gameId_, mode = QStringLiteral("kombat");
    for (int r = 0; r < list_->rowCount(); ++r)
        if (list_->item(r, 0)->text() == code) mode = list_->item(r, 3)->data(Qt::UserRole).toString();
    QJsonObject msg{{QStringLiteral("type"), QStringLiteral("lobby_join")}, {QStringLiteral("lobby"), code},
                    {QStringLiteral("password"), password}, {QStringLiteral("character"), savedCharacter(game, mode)},
                    {QStringLiteral("spectate"), spectate}, {QStringLiteral("link"), server_->linkType()}};
    if (controller_->serverRttMs() >= 0) msg.insert(QStringLiteral("server_rtt"), controller_->serverRttMs());
    server_->send(msg);
}

void LobbyPage::sendChat() {
    const QString text = chatInput_->text().trimmed();
    if (text.isEmpty() || lobby_.isEmpty()) return;
    server_->send({{QStringLiteral("type"), QStringLiteral("lobby_chat")}, {QStringLiteral("text"), text}});
    chatInput_->clear();
}

void LobbyPage::appendChat(const QString& from, const QString& text, double ts) {
    const QString when = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ts * 1000)).toString(QStringLiteral("hh:mm"));
    // a line from the lobby itself (King of the Hill: who is king, who is next, Respect Points) has no name
    chat_->appendPlainText(from.isEmpty() ? QStringLiteral("%1  -- %2").arg(when, text)
                                          : QStringLiteral("%1  %2: %3").arg(when, from, text));
}

void LobbyPage::rate(int score) {
    const QJsonObject r = lobby_.value(QStringLiteral("rating")).toObject();
    if (r.isEmpty() || !server_->isLoggedIn()) return;
    ratedMatch_ = s(r, "match");
    server_->send({{QStringLiteral("type"), QStringLiteral("lobby_rate")}, {QStringLiteral("match"), ratedMatch_},
                   {QStringLiteral("score"), score}});
    ratePanel_->hide();
    if (ready_->isVisible() && ready_->isEnabled()) ready_->setFocus(Qt::OtherFocusReason);  // a controller goes on
}

void LobbyPage::onMessage(const QJsonObject& msg) {
    const QString type = s(msg, "type");
    if (type == QLatin1String("lobbies")) {
        const QJsonArray items = msg.value(QStringLiteral("lobbies")).toArray();
        list_->setRowCount(static_cast<int>(items.size()));
        const Game* g = GameData::instance().game(gameId_);
        for (int i = 0; i < items.size(); ++i) {
            const QJsonObject lb = items.at(i).toObject();
            const GameMode* m = g ? g->mode(s(lb, "mode")) : nullptr;
            const bool hill = lb.value(QStringLiteral("koth")).toBool();
            const int players = lb.value(QStringLiteral("players")).toInt();
            const int inLine = lb.value(QStringLiteral("in_line")).toInt();
            const int watching = lb.value(QStringLiteral("spectators")).toInt() - inLine;  // the rest watch only
            QString who = QStringLiteral("%1/2").arg(players);
            if (inLine > 0) who += tr(", %1 in line").arg(inLine);
            if (watching > 0) who += tr(", %1 watching").arg(watching);
            const QString modeName = m ? m->name : s(lb, "mode");
            const QStringList cells = {s(lb, "id"), s(lb, "name"), s(lb, "host"),
                                       hill ? tr("%1, King of the Hill").arg(modeName) : modeName,
                                       GameData::regionName(s(lb, "region")), who,
                                       lb.value(QStringLiteral("locked")).toBool() ? tr("locked") : QString()};
            for (int c = 0; c < cells.size(); ++c) {
                auto* item = new QTableWidgetItem(cells[c]);
                if (c == 3) item->setData(Qt::UserRole, s(lb, "mode"));
                // full: a double click watches it, but King of the Hill has a line to wait in
                if (c == 5)
                    item->setData(Qt::UserRole,
                                  !hill && (players >= 2 || s(lb, "state") == QLatin1String("in_match")));
                list_->setItem(i, c, item);
            }
        }
        listInfo_->setText(items.isEmpty() ? tr("No open lobbies for this game.") : tr("%1 lobbies").arg(items.size()));
    } else if (type == QLatin1String("lobby")) {
        showLobby(msg.value(QStringLiteral("lobby")).toObject());
    } else if (type == QLatin1String("lobby_left")) {
        // the player left (the server says so only then; a dropped connection keeps the place, and the game): the
        // lobby's game is not needed any more
        leftLobby();
        controller_->closeGameWhenIdle();
        refresh();
    } else if (type == QLatin1String("lobby_chat")) {
        if (s(msg, "lobby") == s(lobby_, "id"))
            appendChat(s(msg, "from"), s(msg, "text"), msg.value(QStringLiteral("ts")).toDouble());
    } else if (type == QLatin1String("lobby_rated")) {
        const int score = msg.value(QStringLiteral("score")).toInt();
        chat_->appendPlainText(score == 1 ? tr("-- You gave %1 1 Respect Point.").arg(s(msg, "winner"))
                                          : tr("-- You gave %1 %2 Respect Points.").arg(s(msg, "winner")).arg(score));
    } else if (type == QLatin1String("error") && s(msg, "re").startsWith(QLatin1String("lobby_"))) {
        const QString text = s(msg, "message");
        if (lobby_.isEmpty())
            listInfo_->setText(text);
        else
            chat_->appendPlainText(tr("-- %1").arg(text));
        if (s(msg, "re") == QLatin1String("lobby_ready")) ready_->setChecked(false);
    }
}

void LobbyPage::showLobby(const QJsonObject& lobby) {
    const bool newLobby = s(lobby, "id") != s(lobby_, "id");
    lobby_ = lobby;
    right_->setCurrentIndex(1);
    const Game* g = GameData::instance().game(s(lobby, "game"));
    const GameMode* m = g ? g->mode(s(lobby, "mode")) : nullptr;
    const bool hill = koth();
    lobbyTitle_->setText(QStringLiteral("%1   [%2]").arg(s(lobby, "name"), s(lobby, "id")));
    QJsonArray members = lobby.value(QStringLiteral("members")).toArray();
    int playersIn = 0, watching = 0, inLine = 0;
    for (const auto& mv : members) {
        const QJsonObject mem = mv.toObject();
        if (!mem.value(QStringLiteral("spectator")).toBool()) playersIn++;
        else if (mem.value(QStringLiteral("line")).toInt() > 0) inLine++;
        else watching++;
    }
    QString extras;
    if (hill) extras += tr("  |  King of the Hill");
    if (m && m->deathTraps && !lobby.value(QStringLiteral("death_traps")).toBool(true)) extras += tr("  |  no death traps");
    if (inLine) extras += tr("  |  %1 in line").arg(inLine);
    if (watching) extras += tr("  |  %1 watching").arg(watching);
    lobbyInfo_->setText(tr("%1  |  %2  |  best of %3  |  %4%5  |  %6%7")
                            .arg(g ? g->shortName : s(lobby, "game"), m ? m->name : s(lobby, "mode"))
                            .arg(lobby.value(QStringLiteral("rounds")).toInt())
                            .arg(GameData::regionName(s(lobby, "region")),
                                 lobby.value(QStringLiteral("locked")).toBool() ? tr(", password") : QString(),
                                 s(lobby, "state").replace(QLatin1Char('_'), QLatin1Char(' ')), extras));
    // King of the Hill: the two players (the king first), then the line in its order, then who watches only
    const QString king = s(lobby, "king");
    if (hill) {
        QList<QJsonObject> sorted;
        for (const auto& mv : members) sorted << mv.toObject();
        const auto rank = [&king](const QJsonObject& mem) {
            if (!mem.value(QStringLiteral("spectator")).toBool()) return s(mem, "name") == king ? 0 : 1;
            const int line = mem.value(QStringLiteral("line")).toInt();
            return line > 0 ? 1 + line : 100;
        };
        std::stable_sort(sorted.begin(), sorted.end(),
                         [&rank](const QJsonObject& a, const QJsonObject& b) { return rank(a) < rank(b); });
        members = {};
        for (const QJsonObject& mem : sorted) members.append(mem);
    }
    members_->setRowCount(static_cast<int>(members.size()));
    members_->setColumnHidden(kRespect, !hill);
    members_->setMaximumHeight(hill ? 260 : 170);
    for (int i = 0; i < members.size(); ++i) {
        const QJsonObject mem = members.at(i).toObject();
        const bool ready = mem.value(QStringLiteral("ready")).toBool();
        const bool spectator = mem.value(QStringLiteral("spectator")).toBool();
        const int line = mem.value(QStringLiteral("line")).toInt();
        // the member's connection (its own column): the link's icon, and their ping to the server
        const QString link = s(mem, "link");
        const int ping = mem.value(QStringLiteral("ping")).toInt(-1);
        const QString name = s(mem, "name") + (mem.value(QStringLiteral("host")).toBool() ? tr("  (host)") : QString());
        const QString fighter = s(mem, "character");
        QString status = spectator ? tr("WATCHING") : ready ? tr("READY") : tr("-");
        if (hill && !spectator && s(mem, "name") == king) status = ready ? tr("KING, READY") : tr("KING");
        else if (hill && line == 1) status = tr("NEXT");
        else if (hill && line > 1) status = tr("IN LINE, %1").arg(line);
        const bool showFighter = !spectator || line > 0;  // King of the Hill's line picks for its turn
        const QStringList cells = {name,
                                   !showFighter ? QString()
                                   : fighter.isEmpty() || fighter == QLatin1String("-")
                                       ? tr("picks in the game")
                                       : (g ? g->characterName(fighter) : fighter),
                                   status,
                                   ping >= 0 ? tr("%1 ms").arg(ping) : QString(),
                                   QString::number(mem.value(QStringLiteral("respect")).toInt())};
        for (int c = 0; c < cells.size(); ++c) members_->setItem(i, c, new QTableWidgetItem(cells[c]));
        members_->item(i, kRespect)->setTextAlignment(Qt::AlignCenter);
        // their rank in the lobby's game and mode (the server's title; someone new to ranked holds the starting one):
        // its badge before the name
        const QString title = s(mem, "title");
        if (const QPixmap badge = style::rankBadge(title, kBadgeHeight); !badge.isNull()) {
            members_->item(i, kPlayer)->setData(Qt::DecorationRole, badge);
            members_->item(i, kPlayer)->setToolTip(tr("Rank: %1").arg(title));
        }
        if (const QPixmap icon = style::linkIcon(link, 16); !icon.isNull()) {
            members_->item(i, kConnection)->setIcon(QIcon(icon));
            members_->item(i, kConnection)->setToolTip(style::linkName(link));
        }
    }
    if (g && (newLobby || character_->count() == 0)) {
        const QSignalBlocker block(character_);
        character_->clear();
        // the game's own select screen, with the player's own controller (cfg::pickInGame); Online Practice and Chess
        // Kombat's teams always
        if (m && m->picksInGame()) {
            character_->addItem(tr("Your team: built in the game"), QString());
        } else {
            character_->addItem(tr("Pick in the game"), QString());
            if (!(m && m->lobbyOnly))
                for (const auto& c : g->roster(s(lobby, "mode"))) character_->addItem(c.name, c.id);
        }
    }
    QString myChar = s(me(), "character");
    if (myChar == QLatin1String("-")) myChar.clear();
    const int ci = character_->findData(myChar);
    if (ci >= 0) {
        const QSignalBlocker block(character_);
        character_->setCurrentIndex(ci);
    }
    const bool meWatching = spectating();
    watchingHere_ = meWatching;
    // a player now: nothing more to watch. King of the Hill calls a spectator up while their game may still show the
    // match just played: it plays its end, and Ready waits for it (updateControls)
    if (!meWatching && !hill) controller_->stopWatching();
    else if (meWatching) rejoinGrace_->stop();  // back in after a dropped connection
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool inMatch = s(lobby, "state") == QLatin1String("in_match");
    const double startsIn = lobby.value(QStringLiteral("starts_in")).toDouble(-1);
    startsAtMs_ = !inMatch && startsIn >= 0 ? now + qint64(startsIn * 1000.0) : 0;
    const QJsonObject r = lobby.value(QStringLiteral("rating")).toObject();
    rateEndsMs_ = r.isEmpty() ? 0 : now + qint64(r.value(QStringLiteral("seconds")).toDouble() * 1000.0);
    const double readyBy = lobby.value(QStringLiteral("ready_by")).toDouble(-1);
    readyByMs_ = hill && readyBy >= 0 ? now + qint64(readyBy * 1000.0) : 0;
    updateControls();
    updateStatus();
    {
        const QSignalBlocker block(deathTraps_);
        deathTraps_->setChecked(lobby.value(QStringLiteral("death_traps")).toBool(true));
    }
    const bool host = s(lobby, "host") == server_->accountName();
    deathTraps_->setVisible(m && m->deathTraps);
    deathTraps_->setEnabled(host && !inMatch);
    deathTraps_->setToolTip(host ? tr("For this lobby's matches. Changing it between matches has both players press "
                                      "Ready again.")
                                 : tr("The host chooses this for the lobby's matches."));
    if (newLobby) {
        chat_->clear();
        ratedMatch_.clear();
        for (const auto& cv : lobby.value(QStringLiteral("chat")).toArray()) {
            const QJsonObject c = cv.toObject();
            appendChat(s(c, "from"), s(c, "text"), c.value(QStringLiteral("ts")).toDouble());
        }
        const int myLine = me().value(QStringLiteral("line")).toInt();
        chat_->appendPlainText(
            hill && myLine > 0
                ? tr("-- Joined King of the Hill lobby %1: you are number %2 in line, and watch until your turn.")
                      .arg(s(lobby, "id")).arg(myLine)
            : meWatching ? tr("-- Watching lobby %1: each of its matches plays in your game.").arg(s(lobby, "id"))
            : m && m->lobbyOnly
                ? tr("-- Online Practice, lobby %1: share the code with one friend (two players, nobody watching). No "
                     "timer, health refills, nobody is knocked out. Press BACK in the game (Backspace on a keyboard) to "
                     "go back to character select.").arg(s(lobby, "id"))
            : hill ? tr("-- Joined King of the Hill lobby %1: the winner stays on, the loser goes to the back of the "
                        "line. Share the code.").arg(s(lobby, "id"))
                   : tr("-- Joined lobby %1. Share the code with your opponent.").arg(s(lobby, "id")));
    }
}

void LobbyPage::updateControls() {
    const Game* g = GameData::instance().game(s(lobby_, "game"));
    const GameMode* m = g ? g->mode(s(lobby_, "mode")) : nullptr;
    const bool hill = koth();
    const QJsonObject mine = me();
    const bool meWatching = mine.value(QStringLiteral("spectator")).toBool();
    const int myLine = mine.value(QStringLiteral("line")).toInt();
    const bool inMatch = s(lobby_, "state") == QLatin1String("in_match");
    int playersIn = 0;
    for (const auto& mv : lobby_.value(QStringLiteral("members")).toArray())
        if (!mv.toObject().value(QStringLiteral("spectator")).toBool()) playersIn++;
    const bool meReady = mine.value(QStringLiteral("ready")).toBool();
    ready_->setChecked(meReady);
    // the game still plays (or shows) the last match: it takes the next one once back at its hold point. Un-readying
    // stays open (Online Practice's way out between its matches)
    ready_->setEnabled(!inMatch && (meReady || !controller_->inMatch()));
    ready_->setVisible(!meWatching);
    // King of the Hill's line picks a fighter for its turn meanwhile; Online Practice picks in the game
    const bool picks = !meWatching || (hill && myLine > 0);
    character_->setEnabled(!(m && (m->picksInGame() || m->lobbyOnly)) && (meWatching || !inMatch));
    character_->setVisible(picks);
    role_->setVisible(!(m && m->lobbyOnly));  // Online Practice: its two players alone, nobody watches
    if (hill) {
        if (!meWatching) {
            roleAction_ = QStringLiteral("spectator");
            role_->setText(tr("Watch"));
            role_->setToolTip(tr("Step out of the fight and out of the line, and watch this lobby's matches"));
            role_->setEnabled(!inMatch);
        } else if (myLine > 0) {
            roleAction_ = QStringLiteral("spectator");
            role_->setText(tr("Leave line"));
            role_->setToolTip(tr("Watch only: you will not be called up to play"));
            role_->setEnabled(true);
        } else {
            roleAction_ = QStringLiteral("player");
            role_->setText(tr("Get in line"));
            role_->setToolTip(tr("Wait in line for your turn to play the king (at once when a place is free and "
                                 "nobody waits)"));
            role_->setEnabled(true);
        }
    } else {
        // a spectator takes a free place to play between matches; a player steps aside to watch
        roleAction_ = meWatching ? QStringLiteral("player") : QStringLiteral("spectator");
        role_->setText(meWatching ? tr("Play") : tr("Watch"));
        role_->setToolTip(meWatching ? tr("Take a free place to play in this lobby's next match")
                                     : tr("Step aside and watch this lobby's matches in your game"));
        role_->setEnabled(meWatching ? playersIn < 2 && !inMatch : !inMatch);
    }
}

void LobbyPage::updateRating(qint64 now) {
    // the last match's spectators rate its winner, once each, while the rating is open
    const QJsonObject r = lobby_.value(QStringLiteral("rating")).toObject();
    const QString mine = server_->accountName();
    const bool may = !r.isEmpty() && rateEndsMs_ > now && s(r, "match") != ratedMatch_ &&
                     r.value(QStringLiteral("raters")).toArray().contains(mine) &&
                     !r.value(QStringLiteral("rated")).toArray().contains(mine);
    if (may)
        rateLabel_->setText(tr("Rate %1's win, 1 to 10 (%2 s)").arg(s(r, "winner")).arg(secondsLeft(rateEndsMs_, now)));
    if (ratePanel_->isHidden() == may) {
        const bool hadFocus = ratePanel_->isAncestorOf(focusWidget());
        ratePanel_->setVisible(may);
        if (!may && hadFocus) focusReady();
    }
}

QString LobbyPage::kothText(qint64 now) const {
    const QString mine = server_->accountName();
    const QString king = s(lobby_, "king");
    const QJsonObject r = lobby_.value(QStringLiteral("rating")).toObject();
    const QJsonObject self = me();
    const bool meWatching = self.value(QStringLiteral("spectator")).toBool();
    const int myLine = self.value(QStringLiteral("line")).toInt();
    const bool inMatch = s(lobby_, "state") == QLatin1String("in_match");
    QString opponent;
    for (const auto& mv : lobby_.value(QStringLiteral("members")).toArray()) {
        const QJsonObject mem = mv.toObject();
        if (!mem.value(QStringLiteral("spectator")).toBool() && s(mem, "name") != mine) opponent = s(mem, "name");
    }
    QStringList parts;
    // the rating row says it to one who rates
    if (!r.isEmpty() && !inMatch && ratePanel_->isHidden())
        parts << tr("Spectators are rating %1's win: the next match starts once they have (%2 s at most).")
                     .arg(s(r, "winner")).arg(secondsLeft(rateEndsMs_, now));
    if (!meWatching) {
        if (!inMatch) {  // a match under way: its countdown label says so
            const int streak = lobby_.value(QStringLiteral("streak")).toInt();
            const QString wins = streak == 1 ? tr("1 win") : tr("%1 wins in a row").arg(streak);
            if (mine == king)
                parts << (opponent.isEmpty() ? tr("You are the king (%1): waiting for a challenger.").arg(wins)
                                             : tr("You are the king (%1): press Ready to face %2.").arg(wins, opponent));
            else if (!king.isEmpty())
                parts << tr("You are up against the king, %1: press Ready.").arg(king);
            else
                parts << (opponent.isEmpty() ? tr("Waiting for an opponent.")
                                             : tr("Press Ready to play %1: the winner is the king.").arg(opponent));
            if (controller_->inMatch())  // the deadline means little before Ready can be pressed
                parts << tr("Your game is still showing the last match: Ready comes on once it is done.");
            else if (readyByMs_ && !self.value(QStringLiteral("ready")).toBool())
                parts << tr("%1 s left to ready, or the next in line plays instead.")
                             .arg(secondsLeft(readyByMs_, now));
        }
    } else if (myLine == 1) {
        parts << (inMatch ? tr("You are next: you play the winner of this match.") : tr("You are next in line."));
    } else if (myLine > 1) {
        parts << tr("You are number %1 in line.").arg(myLine);
    } else {
        parts << tr("You are watching: Get in line to play the king.");
    }
    return parts.join(QLatin1Char(' '));
}

void LobbyPage::updateStatus() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool hill = koth();
    updateRating(now);
    const bool counting = startsAtMs_ || (hill && (rateEndsMs_ > now || readyByMs_ > now));
    if (counting && !tick_->isActive()) tick_->start();
    else if (!counting) tick_->stop();
    if (s(lobby_, "state") == QLatin1String("in_match")) {
        countdown_->setText(tr("MATCH IN PROGRESS"));
        countdown_->show();
        // King of the Hill: where a spectator stands in line meanwhile
        nextInfo_->setText(hill ? kothText(now) : QString());
        nextInfo_->setVisible(!nextInfo_->text().isEmpty());
        return;
    }
    if (startsAtMs_) {
        const qint64 left = startsAtMs_ - now;
        countdown_->setText(left > 0 ? tr("STARTING IN %1").arg((left + 999) / 1000) : tr("STARTING..."));
        countdown_->show();
        nextInfo_->hide();
        return;
    }
    countdown_->hide();
    nextInfo_->setText(hill            ? kothText(now)
                       : spectating() ? tr("The next match plays in your game once both players are ready.")
                       : lobbyOnly()  ? tr("Practice starts by itself once both players are ready, and again each "
                                           "time someone presses BACK in the game for character select.")
                                      : tr("The match starts by itself once both players are ready."));
    nextInfo_->show();
}

void LobbyPage::leftLobby(bool keepWatching) {
    if (!keepWatching) controller_->stopWatching();
    watchingHere_ = false;
    tick_->stop();
    startsAtMs_ = rateEndsMs_ = readyByMs_ = 0;
    ratedMatch_.clear();
    ratePanel_->hide();
    lobby_ = {};
    right_->setCurrentIndex(0);
    character_->clear();
    chat_->clear();
}
