#include "LobbyPage.h"

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
#include <QVBoxLayout>

#include "AppPaths.h"
#include "Dialogs.h"
#include "GameData.h"
#include "MatchController.h"
#include "ServerClient.h"

namespace {
QString s(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }

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
}  // namespace

LobbyPage::LobbyPage(ServerClient* server, MatchController* controller, QWidget* parent)
    : Page(parent), server_(server), controller_(controller) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 20, 28, 16);
    root->setSpacing(14);
    root->addWidget(ui::pageHeader(tr("Lobbies"), tr("Private or public rooms for two: pick fighters, chat, "
                                                     "ready up, and the host starts. Lobby matches count as casual.")));
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
    joinRow->addWidget(listInfo_, 1);
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
    lobbyInfo_ = ui::label(QString(), "Muted");
    lobbyInfo_->setWordWrap(true);
    pv->addWidget(lobbyTitle_);
    pv->addWidget(lobbyInfo_);
    members_ = makeTable({tr("Player"), tr("Fighter"), tr("Ready")});
    members_->setMaximumHeight(110);
    pv->addWidget(members_);
    auto* controls = new QHBoxLayout;
    character_ = new QComboBox;
    character_->setMinimumWidth(150);
    ready_ = new QPushButton(tr("Ready"));
    ready_->setCheckable(true);
    start_ = new QPushButton(tr("START"));
    start_->setObjectName(QStringLiteral("Primary"));
    leave_ = new QPushButton(tr("Leave"));
    controls->addWidget(character_, 1);
    controls->addWidget(ready_);
    controls->addWidget(leave_);
    pv->addLayout(controls);
    pv->addWidget(start_);
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
    connect(list_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        join(list_->item(row, 0)->text(), !list_->item(row, 6)->text().isEmpty());
    });
    connect(leave_, &QPushButton::clicked, this, [this] { server_->send({{QStringLiteral("type"), QStringLiteral("lobby_leave")}}); });
    connect(start_, &QPushButton::clicked, this, [this] { server_->send({{QStringLiteral("type"), QStringLiteral("lobby_start")}}); });
    connect(ready_, &QPushButton::clicked, this, [this](bool on) {
        if (on) {
            QString err;
            if (!controller_->prepareGame(s(lobby_, "game"), &err)) {
                ready_->setChecked(false);
                QMessageBox::warning(this, tr("Ready"), err);
                return;
            }
        }
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_ready")}, {QStringLiteral("ready"), on},
                       {QStringLiteral("delay"), cfg::inputDelay()}});
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
    connect(server_, &ServerClient::stateChanged, this, [this](ServerClient::State st) {
        if (st == ServerClient::State::Offline) leftLobby();
        if (st == ServerClient::State::LoggedIn) refresh();
    });
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
    if (cfg::pickInGame() && !(m && m->picksInGame())) return QString();  // the game's own select screen
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
    CreateLobbyDialog dlg(*g, tr("%1's lobby").arg(server_->accountName()), this);
    if (dlg.exec() != QDialog::Accepted) return;
    server_->send({{QStringLiteral("type"), QStringLiteral("lobby_create")},
                   {QStringLiteral("name"), dlg.name()},
                   {QStringLiteral("game"), gameId_},
                   {QStringLiteral("mode"), dlg.mode()},
                   {QStringLiteral("password"), dlg.password()},
                   {QStringLiteral("rounds"), dlg.rounds()},
                   {QStringLiteral("region"), cfg::region()},
                   {QStringLiteral("character"), savedCharacter(gameId_, dlg.mode())}});
}

void LobbyPage::join(const QString& code, bool locked) {
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
    server_->send({{QStringLiteral("type"), QStringLiteral("lobby_join")}, {QStringLiteral("lobby"), code},
                   {QStringLiteral("password"), password}, {QStringLiteral("character"), savedCharacter(game, mode)}});
}

void LobbyPage::sendChat() {
    const QString text = chatInput_->text().trimmed();
    if (text.isEmpty() || lobby_.isEmpty()) return;
    server_->send({{QStringLiteral("type"), QStringLiteral("lobby_chat")}, {QStringLiteral("text"), text}});
    chatInput_->clear();
}

void LobbyPage::appendChat(const QString& from, const QString& text, double ts) {
    const QString when = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ts * 1000)).toString(QStringLiteral("hh:mm"));
    chat_->appendPlainText(QStringLiteral("%1  %2: %3").arg(when, from, text));
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
            const QStringList cells = {s(lb, "id"), s(lb, "name"), s(lb, "host"), m ? m->name : s(lb, "mode"),
                                       GameData::regionName(s(lb, "region")),
                                       QStringLiteral("%1/2").arg(lb.value(QStringLiteral("players")).toInt()),
                                       lb.value(QStringLiteral("locked")).toBool() ? tr("locked") : QString()};
            for (int c = 0; c < cells.size(); ++c) {
                auto* item = new QTableWidgetItem(cells[c]);
                if (c == 3) item->setData(Qt::UserRole, s(lb, "mode"));
                list_->setItem(i, c, item);
            }
        }
        listInfo_->setText(items.isEmpty() ? tr("No open lobbies for this game.") : tr("%1 lobbies").arg(items.size()));
    } else if (type == QLatin1String("lobby")) {
        showLobby(msg.value(QStringLiteral("lobby")).toObject());
    } else if (type == QLatin1String("lobby_left")) {
        leftLobby();
        refresh();
    } else if (type == QLatin1String("lobby_chat")) {
        if (s(msg, "lobby") == s(lobby_, "id"))
            appendChat(s(msg, "from"), s(msg, "text"), msg.value(QStringLiteral("ts")).toDouble());
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
    lobbyTitle_->setText(QStringLiteral("%1   [%2]").arg(s(lobby, "name"), s(lobby, "id")));
    lobbyInfo_->setText(tr("%1  |  %2  |  best of %3  |  %4%5  |  %6")
                            .arg(g ? g->shortName : s(lobby, "game"), m ? m->name : s(lobby, "mode"))
                            .arg(lobby.value(QStringLiteral("rounds")).toInt())
                            .arg(GameData::regionName(s(lobby, "region")),
                                 lobby.value(QStringLiteral("locked")).toBool() ? tr(", password") : QString(),
                                 s(lobby, "state").replace(QLatin1Char('_'), QLatin1Char(' '))));
    const QJsonArray members = lobby.value(QStringLiteral("members")).toArray();
    members_->setRowCount(static_cast<int>(members.size()));
    bool allReady = members.size() == 2;
    bool meReady = false;
    QString myChar;
    for (int i = 0; i < members.size(); ++i) {
        const QJsonObject mem = members.at(i).toObject();
        const bool ready = mem.value(QStringLiteral("ready")).toBool();
        allReady = allReady && ready;
        if (s(mem, "name") == server_->accountName()) {
            meReady = ready;
            myChar = s(mem, "character");
        }
        const QString name = s(mem, "name") + (mem.value(QStringLiteral("host")).toBool() ? tr("  (host)") : QString());
        const QString fighter = s(mem, "character");
        const QStringList cells = {name,
                                   fighter.isEmpty() || fighter == QLatin1String("-")
                                       ? tr("picks in the game")
                                       : (g ? g->characterName(fighter) : fighter),
                                   ready ? tr("READY") : tr("-")};
        for (int c = 0; c < cells.size(); ++c) members_->setItem(i, c, new QTableWidgetItem(cells[c]));
    }
    if (g && (newLobby || character_->count() == 0)) {
        const QSignalBlocker block(character_);
        character_->clear();
        // the game's own select screen, with the player's own controller (cfg::pickInGame)
        if (!(m && m->picksInGame())) character_->addItem(tr("Pick in the game"), QString());
        for (const auto& c : g->roster(s(lobby, "mode"))) character_->addItem(c.name, c.id);
    }
    if (myChar == QLatin1String("-")) myChar.clear();
    const int ci = character_->findData(myChar);
    if (ci >= 0) {
        const QSignalBlocker block(character_);
        character_->setCurrentIndex(ci);
    }
    const bool inMatch = s(lobby, "state") == QLatin1String("in_match");
    const bool host = s(lobby, "host") == server_->accountName();
    ready_->setChecked(meReady);
    ready_->setEnabled(!inMatch);
    character_->setEnabled(!inMatch && !(m && m->picksInGame()));
    start_->setVisible(host);
    start_->setEnabled(host && allReady && !inMatch);
    start_->setText(inMatch ? tr("MATCH IN PROGRESS") : tr("START"));
    if (newLobby) {
        chat_->clear();
        for (const auto& cv : lobby.value(QStringLiteral("chat")).toArray()) {
            const QJsonObject c = cv.toObject();
            appendChat(s(c, "from"), s(c, "text"), c.value(QStringLiteral("ts")).toDouble());
        }
        chat_->appendPlainText(tr("-- Joined lobby %1. Share the code with your opponent.").arg(s(lobby, "id")));
    }
}

void LobbyPage::leftLobby() {
    lobby_ = {};
    right_->setCurrentIndex(0);
    character_->clear();
    chat_->clear();
}
