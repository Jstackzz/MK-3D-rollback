#include "MainWindow.h"

#include "Upnp.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "AppPaths.h"
#include "ControlsPage.h"
#include "Dialogs.h"
#include "GameData.h"
#include "GamepadNav.h"
#include "GameProcess.h"
#include "LobbyPage.h"
#include "MatchController.h"
#include "PlayPages.h"
#include "ServerClient.h"
#include "Style.h"
#include "TitleImage.h"
#include "ToolPages.h"
#include "Updater.h"
#include "Widgets.h"
#include "WinUtil.h"

namespace {
enum PageIndex { kRanked, kCasual, kLobby, kSingle, kLeaderboard, kBuild, kControls, kSettings };
const char* kPageNames[] = {"ranked", "casual", "lobby", "single", "leaderboard", "build", "controls", "settings"};
}  // namespace

MainWindow::MainWindow(const LaunchOptions& opts, QWidget* parent)
    : QMainWindow(parent), opts_(opts), server_(new ServerClient(this)), controller_(new MatchController(server_, this)) {
    setWindowTitle(tr("MK3D Online"));
    setWindowIcon(style::appIcon());
    setMinimumSize(1120, 720);
    resize(1320, 820);

    if (!opts_.server.isEmpty()) cfg::set(QStringLiteral("server/address"), opts_.server);
    if (!opts_.fingerprint.isEmpty())
        cfg::set(QStringLiteral("server/fingerprint"), ServerClient::normalizeFingerprint(opts_.fingerprint));
    if (!opts_.practice.isEmpty())
        cfg::set(QStringLiteral("online/practice_while_waiting"), opts_.practice != QLatin1String("off"));
    if (!opts_.deathTraps.isEmpty())
        cfg::set(QStringLiteral("online/death_traps"), opts_.deathTraps != QLatin1String("off"));

    auto* root = new QWidget;
    root->setObjectName(QStringLiteral("Root"));
    auto* v = new QVBoxLayout(root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(buildHeader());
    v->addWidget(buildNav());
    notice_ = new QLabel;
    notice_->setWordWrap(true);
    notice_->hide();
    v->addWidget(notice_);

    stack_ = new QStackedWidget;
    ranked_ = new QueuePage(true, server_, controller_);
    casual_ = new QueuePage(false, server_, controller_);
    settings_ = new SettingsPage(server_);
    build_ = new BuildPage;
    pages_ = {ranked_, casual_, new LobbyPage(server_, controller_), new SinglePlayerPage(controller_),
              new LeaderboardPage(server_), build_, new ControlsPage, settings_};
    for (Page* p : pages_) stack_->addWidget(p);
    v->addWidget(stack_, 1);
    v->addWidget(buildFooter());
    setCentralWidget(root);

    // the game port on the router, as long as the launcher runs (one-PC tests stay on loopback and skip it)
    upnp_ = new UpnpMapper(this);
    const QString standInRouter = cfg::get(QStringLiteral("dev/upnp_location")).toString();
    if (!standInRouter.isEmpty()) upnp_->setLocation(QUrl(standInRouter));
    if (opts_.upnpProbe)
        upnp_->map(static_cast<quint16>(cfg::gamePort()), true);
    else if (cfg::upnp() && !cfg::udpLoopback())
        upnp_->map(static_cast<quint16>(cfg::gamePort()));
    settings_->setUpnp(upnp_);

    connect(settings_, &SettingsPage::reconnectRequested, this, &MainWindow::connectServer);
    connect(casual_, &QueuePage::seriesCard, this, &MainWindow::onKasualSeries);
    // standby: the match's game up for an accepted match, then single player again where it stood
    connect(controller_, &MatchController::gameShown, this, &MainWindow::focusGame);
    connect(controller_, &MatchController::soloResumed, this, [this] {
        GameProcess* solo = controller_->solo();
        const bool front = solo && winutil::bringProcessToFront(solo->pid());
        applog::write(QStringLiteral("popup: single player %1").arg(front ? QStringLiteral("in front again")
                                                                         : QStringLiteral("not brought to the front")));
    });
    connect(controller_, &MatchController::soloStarted, this, &MainWindow::frontSoloWhenUp);
    // the launcher with a controller: LB / RB the pages, START finds a match on a queue page
    pad_ = new GamepadNav(this);
    pad_->onPage = [this](int step) {
        const int n = int(pages_.size());
        selectPage((stack_->currentIndex() + step + n) % n);
    };
    pad_->onStart = [this] {
        if (auto* queue = qobject_cast<QueuePage*>(stack_->currentWidget())) queue->pressFind();
        else if (auto* lobby = qobject_cast<LobbyPage*>(stack_->currentWidget())) lobby->pressReady();
    };
    pad_->paused = [this] { return controller_->inMatch(); };
    soloFrontTimer_.setInterval(400);
    connect(&soloFrontTimer_, &QTimer::timeout, this, [this] {
        GameProcess* solo = controller_->solo();
        const MatchController::Phase p = controller_->phase();
        // not over a match found (its prompt is up) or a match: that froze single player, which comes back by itself
        const bool waiting = p == MatchController::Phase::Searching || p == MatchController::Phase::Solo;
        if (!solo || !waiting || ++soloFrontTries_ > 75) {
            soloFrontTimer_.stop();
            return;
        }
        if (!winutil::bringProcessToFront(solo->pid())) return;  // no window yet
        soloFrontTimer_.stop();
        applog::write(QStringLiteral("popup: %1 in front").arg(controller_->soloIsPractice() ? QStringLiteral("practice")
                                                                                            : QStringLiteral("single player")));
    });
    connect(server_, &ServerClient::stateChanged, this, [this](ServerClient::State st) {
        updateFooter();
        updateAccountButton();
        if (st == ServerClient::State::Online) {
            cliLogin();
            QTimer::singleShot(0, this, &MainWindow::askForAccount);  // out of the socket's signal first
        }
    });
    // in-app updates (Updater): offered from the server's messages, installed once nothing uses the games
    updater_ = new Updater(this);
    connect(updater_, &Updater::progress, this, [this](const QString& text, int percent) {
        if (updateDialog_) updateDialog_->setProgress(text, percent);
    });
    connect(updater_, &Updater::failed, this, [this](const QString& text) {
        installPending_ = false;
        if (updateDialog_) updateDialog_->setFailed(text);
        else showNotice(text, true);
    });
    connect(updater_, &Updater::readyToInstall, this, &MainWindow::installUpdateWhenFree);
    connect(updater_, &Updater::installed, this, [this](const QString& version, bool started) {
        if (!started)
            QMessageBox::information(this, tr("MK3D Online %1").arg(version),
                                     tr("MK3D Online is now %1: start it again.").arg(version));
        close();
    });
    auto whenFree = [this] {
        QTimer::singleShot(0, this, [this] {  // out of the signal
            if (installPending_) {
                installUpdateWhenFree();
            } else if (offerPending_ && gamesBusy().isEmpty() && !updater_->busy()) {
                offerPending_ = false;
                showUpdateDialog(offerRequired_);
            }
        });
    };
    connect(controller_, &MatchController::phaseChanged, this, whenFree);
    connect(controller_, &MatchController::gameStatusChanged, this, whenFree);
    connect(build_, &BuildPage::busyChanged, this, whenFree);
    connect(build_, &BuildPage::busyChanged, this, [this](bool busy) {
        if (!busy) updateGameLogos();  // a game just extracted brings its logo
    });
    Updater::cleanUp();  // what an update set aside, once free; the old launcher may still be closing:
    if (!opts_.updatedFrom.isEmpty())
        for (int s : {2, 5, 10, 20}) QTimer::singleShot(s * 1000, this, [] { Updater::cleanUp(); });
    connect(server_, &ServerClient::message, this, &MainWindow::onServerMessage);
    connect(server_, &ServerClient::connectionError, this, [this](const QString& text) {
        lastError_ = text;
        updateFooter();
        if (text.contains(QLatin1String("fingerprint"))) showNotice(text, true);
    });
    connect(server_, &ServerClient::untrustedCertificate, this, [this](const QString& fp) {
        const auto answer = QMessageBox::question(
            this, tr("Trust this server?"),
            tr("The server at %1 presented a certificate with SHA-256 fingerprint\n\n%2\n\n"
               "Trust it only if this is the fingerprint the server's operator published.")
                .arg(cfg::serverAddress(), ServerClient::prettyFingerprint(fp)));
        if (answer == QMessageBox::Yes) {
            cfg::set(QStringLiteral("server/fingerprint"), fp);
            connectServer();
        }
    });
    connect(server_, &ServerClient::loggedIn, this, [this](const QJsonObject& acc) {
        updateAccountButton();
        updateFooter();
        if (cfg::get(QStringLiteral("account/remember"), true).toBool() && opts_.user.isEmpty()) {
            cfg::set(QStringLiteral("account/token"), acc.value(QStringLiteral("token")).toString());
            cfg::set(QStringLiteral("account/name"), acc.value(QStringLiteral("name")).toString());
        }
        QTimer::singleShot(300, this, &MainWindow::cliQueue);
        QTimer::singleShot(300, this, &MainWindow::cliLobby);
    });
    connect(server_, &ServerClient::resumeFailed, this, [this] {
        cfg::set(QStringLiteral("account/token"), QString());
        showNotice(tr("Your saved login expired; sign in again."), false);
    });
    connect(server_, &ServerClient::kicked, this, [this](const QString& text) {
        showNotice(text + tr(" Reconnect from Settings to sign in here again."), true);
    });
    connect(controller_, &MatchController::matchFound, this, &MainWindow::onMatchFound);
    connect(controller_, &MatchController::phaseChanged, this, [this](MatchController::Phase p, const QString& text) {
        phaseText_->setText(text);
        if (p != MatchController::Phase::Found && matchDialog_) matchDialog_->done(2);
        if (p == MatchController::Phase::Starting) QApplication::alert(this);
        // --lobby: the next match's ready waited for the game to finish the last one
        if (p == MatchController::Phase::Idle && !opts_.lobby.isEmpty() && !cliLastLobby_.isEmpty())
            cliLobbyMessage(cliLastLobby_);
        // a lobby match is over and its game played the ending: the lobby comes back in front, Ready focused, so a
        // player on a controller readies up for the next one (A, or START anywhere on the page)
        if (p == MatchController::Phase::Idle && wasInMatch_) {
            auto* lobby = qobject_cast<LobbyPage*>(pages_[kLobby]);
            if (lobby && lobby->inLobby()) {
                selectPage(kLobby);
                popUp(this);
                lobby->focusReady();
            }
        }
        wasInMatch_ = controller_->inMatch();
    });
    connect(controller_, &MatchController::seriesChanged, this, [this](const QJsonObject& s) {
        if (opts_.queue.isEmpty() || s.value(QStringLiteral("ranked")).toBool() ||
            s.value(QStringLiteral("state")).toString() != QLatin1String("between") ||
            s.value(QStringLiteral("you_ready")).toBool())
            return;
        if (cliRematches_ >= opts_.rematches) {
            if (opts_.leaveSeries) {
                applog::write(QStringLiteral("cli: leaving the series"));
                QTimer::singleShot(2000, this, [this] { controller_->seriesReady(false); });
            }
            return;
        }
        const QStringList& choices = opts_.rematchChoices;
        const QString choice = choices.isEmpty() ? QStringLiteral("rematch")
                                                 : choices.value(qMin(cliRematches_, int(choices.size()) - 1));
        cliRematches_++;
        applog::write(QStringLiteral("cli: rematch %1 of %2 (%3)").arg(cliRematches_).arg(opts_.rematches).arg(choice));
        QTimer::singleShot(2000, this, [this, choice] { controller_->seriesReady(true, choice); });
    });
    connect(controller_, &MatchController::gameStatusChanged, this, [this](const QString& s) {
        gameText_->setText(tr("Game: %1").arg(s));
    });
    connect(controller_, &MatchController::matchCancelled, this, [this](const QString& reason, bool requeued) {
        if (!requeued && reason != QLatin1String("declined")) showNotice(tr("Match cancelled (%1).").arg(reason), false);
    });

    noticeTimer_.setSingleShot(true);
    connect(&noticeTimer_, &QTimer::timeout, notice_, &QWidget::hide);

    QString startGame = opts_.game.isEmpty() ? cfg::get(QStringLiteral("ui/game"), QStringLiteral("mkd")).toString() : opts_.game;
    if (!GameData::instance().game(startGame)) startGame = QStringLiteral("mkd");
    selectGame(startGame);
    int startPage = kRanked;
    const QString pageName = opts_.page.isEmpty() ? cfg::get(QStringLiteral("ui/page"), QStringLiteral("ranked")).toString() : opts_.page;
    for (int i = 0; i < kSettings + 1; ++i)
        if (pageName == QLatin1String(kPageNames[i])) startPage = i;
    if (pageName == QLatin1String("practice")) startPage = kSingle;  // its name before 0.1.3
    if (opts_.queue == QLatin1String("casual")) startPage = kCasual;
    if (opts_.queue == QLatin1String("ranked")) startPage = kRanked;
    if (opts_.localTest) startPage = kSingle;
    if (!opts_.extractImage.isEmpty()) startPage = kBuild;
    selectPage(startPage);
    if (opts_.localTest)
        QTimer::singleShot(500, this, [this] {
            QString err;
            if (!controller_->startLocalTest(gameId_, QStringLiteral("kombat"), QStringLiteral("Scorpion"),
                                             QStringLiteral("SubZero"), &err))
                applog::write(QStringLiteral("cli: local test failed: %1").arg(err));
        });
    if (!opts_.extractImage.isEmpty())
        QTimer::singleShot(500, this, [this] { build_->extractImage(opts_.extractImage); });

    server_->setResumeToken(opts_.user.isEmpty() ? cfg::get(QStringLiteral("account/token")).toString() : QString());
    updateFooter();
    updateAccountButton();
    if (cfg::get(QStringLiteral("server/auto_connect"), true).toBool() || !opts_.server.isEmpty())
        QTimer::singleShot(0, this, &MainWindow::connectServer);
    // Smart App Control blocks every game this launcher starts: say so before the player builds or plays one
    applog::write(QStringLiteral("Smart App Control: %1").arg(winutil::smartAppControl()));
    if (winutil::smartAppControl() == 1) QTimer::singleShot(400, this, &MainWindow::warnSmartAppControl);
    if (!opts_.updatedFrom.isEmpty()) {
        applog::write(QStringLiteral("update: now %1, from %2").arg(QStringLiteral(MK3D_LAUNCHER_VERSION), opts_.updatedFrom));
        showNotice(tr("MK3D Online is now %1 (it was %2). The games you have played prepare their code for it next, "
                      "one at a time: a minute or two each.")
                       .arg(QStringLiteral(MK3D_LAUNCHER_VERSION), opts_.updatedFrom),
                   false);
    }
    // a game whose code an update left behind (or an update unpacked by hand) builds it now, not on its next start
    connect(controller_, &MatchController::codePrepChanged, this, [this](const QString& text, bool) {
        showNotice(text, false);
    });
    QTimer::singleShot(opts_.updatedFrom.isEmpty() ? 1500 : 6000, this, [this] { controller_->prepareStaleCode(); });
}

void MainWindow::warnSmartAppControl() {
    QMessageBox box(QMessageBox::Warning, tr("Smart App Control is on"), GameProcess::smartAppControlHelp(false),
                    QMessageBox::Ok, this);
    QPushButton* open = box.addButton(tr("Open Windows Security"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:windowsdefender")));
}

QWidget* MainWindow::buildHeader() {
    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("Header"));
    header->setAttribute(Qt::WA_StyledBackground);
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(24, 14, 24, 14);
    h->setSpacing(16);
    auto* emblem = new QLabel;
    emblem->setObjectName(QStringLiteral("Emblem"));
    emblem->setPixmap(style::logo(76));
    h->addWidget(emblem);

    auto* titles = new QVBoxLayout;
    titles->setSpacing(0);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(12);
    auto* main = ui::label(QStringLiteral("MK3D"), "TitleMain");
    auto* sub = ui::label(QStringLiteral("ONLINE"), "TitleSub");
    ui::letterSpacing(main, 3);
    ui::letterSpacing(sub, 8);
    titleRow->addWidget(main);
    titleRow->addWidget(sub);
    titleRow->addStretch();
    titles->addLayout(titleRow);
    auto* tag = ui::label(tr("NATIVE PC KOMBAT  ·  ROLLBACK NETPLAY  ·  BUILT FROM YOUR OWN DISC"), "Tagline");
    ui::letterSpacing(tag, 3);
    titles->addWidget(tag);
    h->addLayout(titles);
    h->addStretch();

    auto* right = new QVBoxLayout;
    right->setSpacing(10);
    auto* accountRow = new QHBoxLayout;
    accountRow->addStretch();
    account_ = new QPushButton;
    account_->setObjectName(QStringLiteral("AccountButton"));
    account_->setCursor(Qt::PointingHandCursor);
    connect(account_, &QPushButton::clicked, this, &MainWindow::onAccountButton);
    accountRow->addWidget(account_);
    right->addLayout(accountRow);
    auto* games = new QHBoxLayout;
    games->setSpacing(0);
    gameGroup_ = new QButtonGroup(this);
    gameGroup_->setExclusive(true);
    for (const Game& g : GameData::instance().games()) {
        auto* b = new QPushButton(g.shortName.toUpper());
        b->setObjectName(QStringLiteral("GameButton"));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(QStringLiteral("%1 (%2)").arg(g.title, g.edition));
        b->setProperty("gameId", g.id);
        ui::letterSpacing(b, 1);
        gameGroup_->addButton(b);
        games->addWidget(b);
    }
    connect(gameGroup_, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* b) {
        selectGame(b->property("gameId").toString());
    });
    updateGameLogos();
    right->addLayout(games);
    h->addLayout(right);
    return header;
}

void MainWindow::updateGameLogos() {
    for (QAbstractButton* b : gameGroup_->buttons()) {
        const QImage logo = titleimage::forGame(b->property("gameId").toString());
        const Game* g = GameData::instance().game(b->property("gameId").toString());
        const QString name = g ? g->shortName.toUpper() : b->text().trimmed();
        b->setIcon(logo.isNull() ? QIcon() : QIcon(QPixmap::fromImage(logo)));
        b->setIconSize(QSize(22, 22));
        b->setText(logo.isNull() ? name : QStringLiteral(" ") + name);  // a little room after the logo
    }
}

QWidget* MainWindow::buildNav() {
    auto* nav = new QWidget;
    nav->setObjectName(QStringLiteral("NavBar"));
    nav->setAttribute(Qt::WA_StyledBackground);
    auto* h = new QHBoxLayout(nav);
    h->setContentsMargins(16, 0, 16, 0);
    h->setSpacing(2);
    navGroup_ = new QButtonGroup(this);
    navGroup_->setExclusive(true);
    const QStringList names = {tr("RANKED"), tr("KASUAL"), tr("LOBBY"), tr("SINGLE PLAYER"), tr("LEADERBOARD"),
                               tr("BUILD GAME"), tr("CONTROLS"), tr("SETTINGS")};
    for (int i = 0; i < names.size(); ++i) {
        auto* b = new QPushButton(names[i]);
        b->setObjectName(QStringLiteral("NavButton"));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        ui::letterSpacing(b, 2);
        navGroup_->addButton(b, i);
        if (i == kBuild) h->addStretch();
        h->addWidget(b);
    }
    connect(navGroup_, &QButtonGroup::idClicked, this, &MainWindow::selectPage);
    return nav;
}

QWidget* MainWindow::buildFooter() {
    auto* footer = new QWidget;
    footer->setObjectName(QStringLiteral("Footer"));
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* h = new QHBoxLayout(footer);
    h->setContentsMargins(16, 6, 16, 6);
    h->setSpacing(14);
    connDot_ = new QLabel(QStringLiteral("●"));
    connDot_->setObjectName(QStringLiteral("Dot"));
    connText_ = new QLabel;
    onlineText_ = new QLabel;
    phaseText_ = new QLabel;
    gameText_ = new QLabel(tr("Game: not running"));
    auto* version = new QLabel(QStringLiteral("v" MK3D_LAUNCHER_VERSION));
    updateButton_ = new QPushButton;
    updateButton_->setObjectName(QStringLiteral("Primary"));
    updateButton_->setProperty("compact", true);
    updateButton_->hide();
    connect(updateButton_, &QPushButton::clicked, this, [this] { showUpdateDialog(false); });
    h->addWidget(connDot_);
    h->addWidget(connText_);
    h->addWidget(onlineText_);
    h->addStretch();
    h->addWidget(phaseText_);
    h->addWidget(gameText_);
    h->addWidget(updateButton_);
    h->addWidget(version);
    return footer;
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (!darkApplied_) {
        winutil::useDarkTitleBar(this);
        darkApplied_ = true;
    }
}

void MainWindow::selectGame(const QString& id) {
    gameId_ = id;
    cfg::set(QStringLiteral("ui/game"), id);
    for (QAbstractButton* b : gameGroup_->buttons()) b->setChecked(b->property("gameId").toString() == id);
    for (Page* p : pages_) p->setGame(id);
    const Game* g = GameData::instance().game(id);
    setWindowTitle(g ? tr("MK3D Online - %1").arg(g->title) : tr("MK3D Online"));
}

void MainWindow::selectPage(int index) {
    if (index < 0 || index >= pages_.size()) return;
    stack_->setCurrentIndex(index);
    if (QAbstractButton* b = navGroup_->button(index)) b->setChecked(true);
    cfg::set(QStringLiteral("ui/page"), QLatin1String(kPageNames[index]));
}

void MainWindow::connectServer() {
    lastError_.clear();
    server_->connectTo(cfg::serverAddress(), cfg::fingerprint());
}

void MainWindow::updateFooter() {
    QString color = QStringLiteral("#6A6560"), text;
    switch (server_->state()) {
    case ServerClient::State::Offline:
        color = QStringLiteral("#B3121F");
        text = lastError_.isEmpty() ? tr("Offline") : tr("Offline: %1").arg(lastError_);
        break;
    case ServerClient::State::Connecting:
        color = QStringLiteral("#F0B35A");
        text = tr("Connecting to %1...").arg(cfg::serverAddress());
        break;
    case ServerClient::State::Online:
        color = QStringLiteral("#E8C25A");
        text = tr("Connected to %1 (not signed in)").arg(cfg::serverAddress());
        break;
    case ServerClient::State::LoggedIn:
        color = QStringLiteral("#3FC66B");
        text = tr("Connected to %1 as %2").arg(cfg::serverAddress(), server_->accountName());
        break;
    }
    connDot_->setStyleSheet(QStringLiteral("color: %1;").arg(color));
    connText_->setText(text);
    if (!server_->isOnline()) {
        onlineText_->clear();
    } else if (!online_.isEmpty()) {
        const int queued = online_.value(QStringLiteral("queued")).toInt();
        onlineText_->setText(tr("%1 online  ·  %2 searching  ·  %3 in matches")
                                 .arg(online_.value(QStringLiteral("count")).toInt())
                                 .arg(queued)
                                 .arg(online_.value(QStringLiteral("playing")).toInt()));
    }
}

void MainWindow::updateAccountButton() {
    if (server_->isLoggedIn())
        account_->setText(QStringLiteral("●  %1").arg(server_->accountName()));
    else
        account_->setText(server_->isOnline() ? tr("SIGN IN") : tr("OFFLINE"));
}

void MainWindow::onAccountButton() {
    if (server_->isLoggedIn()) {
        QMenu menu(this);
        menu.addAction(tr("Signed in as %1").arg(server_->accountName()))->setEnabled(false);
        menu.addSeparator();
        QAction* out = menu.addAction(controller_->inMatch() ? tr("Sign out (after the match)") : tr("Sign out"));
        out->setEnabled(!controller_->inMatch());
        if (menu.exec(account_->mapToGlobal(QPoint(0, account_->height()))) == out) {
            server_->logout();
            cfg::set(QStringLiteral("account/token"), QString());
        }
        return;
    }
    if (!server_->isOnline()) {
        selectPage(kSettings);
        connectServer();
        return;
    }
    openAccountDialog(false);
}

// a launcher that never signed in on this PC opens the account window by itself the first time it is connected in a
// run, to make one; not for the command line's sign-in or the development runs that open with a task
void MainWindow::askForAccount() {
    if (askedForAccount_ || !opts_.user.isEmpty() || opts_.localTest || !opts_.extractImage.isEmpty()) return;
    if (server_->state() != ServerClient::State::Online) return;
    if (!cfg::get(QStringLiteral("account/token")).toString().isEmpty() ||
        !cfg::get(QStringLiteral("account/name")).toString().isEmpty())
        return;
    askedForAccount_ = true;
    applog::write(QStringLiteral("account: none on this PC yet, asking for one"));
    openAccountDialog(true);
}

void MainWindow::openAccountDialog(bool firstTime) {
    LoginDialog dlg(server_, cfg::get(QStringLiteral("account/name")).toString(), this);
    if (firstTime) dlg.askForNewAccount();
    winutil::useDarkTitleBar(&dlg);
    if (dlg.exec() == QDialog::Accepted) {
        cfg::set(QStringLiteral("account/remember"), dlg.remember());
        cfg::set(QStringLiteral("account/name"), dlg.name());
        cfg::set(QStringLiteral("account/token"), dlg.remember() ? server_->account().value(QStringLiteral("token")).toString()
                                                                 : QString());
    }
}

void MainWindow::onServerMessage(const QJsonObject& msg) {
    const QString type = msg.value(QStringLiteral("type")).toString();
    if (!opts_.lobby.isEmpty()) cliLobbyMessage(msg);
    if (type == QLatin1String("welcome") && msg.contains(QStringLiteral("update")))
        offerUpdate(msg.value(QStringLiteral("update")), false);
    else if (type == QLatin1String("update"))
        offerUpdate(msg, false);
    else if (type == QLatin1String("error") && msg.value(QStringLiteral("code")).toString() == QLatin1String("client_outdated"))
        offerUpdate(msg.value(QStringLiteral("update")), true);
    if (type == QLatin1String("online")) {
        online_ = msg;
        updateFooter();
    } else if (type == QLatin1String("welcome")) {
        online_ = QJsonObject{{QStringLiteral("count"), msg.value(QStringLiteral("online"))}};
        updateFooter();
        const QString motd = msg.value(QStringLiteral("motd")).toString();
        if (!motd.isEmpty()) showNotice(motd, false);
        server_->send({{QStringLiteral("type"), QStringLiteral("online")}});
    } else if (type == QLatin1String("notice")) {
        showNotice(msg.value(QStringLiteral("text")).toString(), msg.value(QStringLiteral("level")).toString() == QLatin1String("warn"));
    } else if (cliLoginId_ >= 0 && msg.value(QStringLiteral("id")).toInt(-2) == cliLoginId_) {
        cliLoginId_ = -1;
        if (type == QLatin1String("error")) {
            const QString code = msg.value(QStringLiteral("code")).toString();
            applog::write(QStringLiteral("cli: login failed: %1").arg(code));
            if (code == QLatin1String("name_taken") && !cliTriedRegister_) {
                cliTriedRegister_ = true;
                const QString pw = QProcessEnvironment::systemEnvironment().value(opts_.passwordEnv);
                cliLoginId_ = server_->login(opts_.user, pw);
            }
        }
    }
}

void MainWindow::onMatchFound(const QJsonObject& found) {
    if (!found.value(QStringLiteral("accept_required")).toBool()) {
        const QString opponent = found.value(QStringLiteral("opponent")).toObject().value(QStringLiteral("name")).toString();
        showNotice(found.contains(QStringLiteral("series")) ? tr("Next match against %1 starting.").arg(opponent)
                                                            : tr("Lobby match starting against %1.").arg(opponent),
                   false);
        if (rematchDialog_) {  // both chose: back to the game for it
            rematchDialog_->close();
            focusGame();
        }
        return;
    }
    const Game* g = GameData::instance().game(found.value(QStringLiteral("game")).toString());
    const GameMode* m = g ? g->mode(found.value(QStringLiteral("mode")).toString()) : nullptr;
    const QueueRequest req = controller_->lastRequest();
    if (matchDialog_) matchDialog_->done(2);
    auto* dlg = new MatchFoundDialog(found, g ? g->title : QString(), m ? m->name : QString(),
                                     g ? g->characterName(req.character) : req.character, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    GamepadNav::keepFromBack(dlg);  // B, an attack button, never declines
    // over the games, and in front again whenever a game window takes the front: a match found the moment the search
    // began pops up before the match's game (or practice) opens its window, which covered it for the rest of its
    // countdown, the controller's A going to the game
    dlg->setWindowFlag(Qt::WindowStaysOnTopHint);
    auto* keepFront = new QTimer(dlg);
    keepFront->setInterval(400);
    connect(keepFront, &QTimer::timeout, dlg, [dlg, pulls = 0]() mutable {
        if (!dlg->isVisible() || winutil::inFront(dlg)) return;
        const bool front = winutil::bringToFront(dlg);
        if (++pulls <= 3)
            applog::write(QStringLiteral("popup: Match found %1 again").arg(front ? QStringLiteral("in front")
                                                                                 : QStringLiteral("not in front")));
    });
    keepFront->start();
    matchDialog_ = dlg;
    connect(dlg, &QDialog::finished, this, [this](int result) {
        if (controller_->phase() != MatchController::Phase::Found) return;
        if (result == QDialog::Accepted) {
            controller_->accept();
            focusGame();  // the match comes to the game: no switching back to it
        } else if (result == QDialog::Rejected) {
            controller_->decline();
        }
    });
    if (opts_.autoAcceptSeconds >= 0)
        QTimer::singleShot(opts_.autoAcceptSeconds * 1000, dlg, [dlg] { dlg->accept(); });
    // heard over the game the player waits in (tools/py/make_chime.py)
    if (cfg::matchSound())
        applog::write(winutil::playWav(QStringLiteral(":/sounds/match-found.wav"))
                          ? QStringLiteral("sound: match found chime")
                          : QStringLiteral("sound: the match found chime could not play"));
    popUp(dlg);  // over the game the player waits in
    QApplication::alert(this);
}

void MainWindow::onKasualSeries(const QJsonObject& series, const QString& title, const QString& score,
                                const QString& info, bool canChoose, bool canSelect) {
    const QString state = series.value(QStringLiteral("state")).toString();
    if (state != QLatin1String("between")) {
        if (rematchDialog_) {
            rematchDialog_->close();
            // it ended without a next match (no rematch in time, the other left): the launcher says so, unless
            // single player comes back in front of it (standby)
            if (series.isEmpty() && !controller_->soloRunning()) popUp(this);
        }
        return;
    }
    const QString key = series.value(QStringLiteral("series")).toString() + QLatin1Char('/') +
                        QString::number(series.value(QStringLiteral("played")).toInt());
    // once per match, when the game has played its end (the finisher, a fatality, the winner's pose) and is ready
    if (!rematchDialog_ && rematchShownFor_ != key && controller_->phase() == MatchController::Phase::Idle) {
        rematchShownFor_ = key;
        auto* dlg = new RematchDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        GamepadNav::keepFromBack(dlg);
        rematchDialog_ = dlg;
        connect(dlg, &RematchDialog::chose, this, [this](const QString& choice) {
            if (choice == QLatin1String("leave")) {
                casual_->leaveSeries();
                if (rematchDialog_) rematchDialog_->close();
            } else {
                casual_->choose(choice);  // the prompt stays, waiting for the other player, until the match starts
            }
        });
        dlg->update(title, score, info, canChoose, canSelect);
        popUp(dlg);
        QApplication::alert(this);
        return;
    }
    if (rematchDialog_) rematchDialog_->update(title, score, info, canChoose, canSelect);
}

void MainWindow::popUp(QWidget* dialog) {
    if (isMinimized()) showNormal();  // a dialog of a minimized window stays hidden
    const bool front = winutil::bringToFront(dialog);
    applog::write(QStringLiteral("popup: %1 %2").arg(dialog->windowTitle(), front ? QStringLiteral("in front")
                                                                                 : QStringLiteral("not in front")));
}

void MainWindow::frontSoloWhenUp() {
    soloFrontTries_ = 0;
    soloFrontTimer_.start();
}

void MainWindow::focusGame() {
    GameProcess* g = controller_->game();
    if (!g || !g->pid()) return;
    const bool front = winutil::bringProcessToFront(g->pid());
    applog::write(QStringLiteral("popup: the game %1").arg(front ? QStringLiteral("in front again")
                                                                 : QStringLiteral("not brought to the front")));
}

// once a run (LATER leaves the footer's button to come back to it), every time when this launcher is too old for the
// server; --auto-update installs it without asking (tests)
void MainWindow::offerUpdate(const QJsonValue& value, bool required) {
    const QJsonObject release = Updater::usable(value);
    if (release.isEmpty()) return;
    const QString version = release.value(QStringLiteral("version")).toString();
    applog::write(QStringLiteral("update: %1 offered%2").arg(version, required ? QStringLiteral(", needed to play online") : QString()));
    update_ = release;
    updateButton_->setText(tr("UPDATE TO %1").arg(version));
    updateButton_->show();
    if (updater_->busy()) return;
    if (opts_.autoUpdate && Updater::blocker().isEmpty()) {
        startUpdate();
        return;
    }
    if (updateDialog_ || (updateDismissed_ == version && !required)) return;
    if (!gamesBusy().isEmpty()) {  // not over a match or a game: once it is over
        offerPending_ = true;
        offerRequired_ = offerRequired_ || required;
        return;
    }
    showUpdateDialog(required);
}

QString MainWindow::gamesBusy() const {
    if (controller_->soloRunning()) return tr("you close your single-player game (Single player, Close the game)");
    if (controller_->phase() != MatchController::Phase::Idle) return tr("this is over: %1").arg(controller_->phaseText());
    if (build_->busy()) return tr("Build game is done");
    if (controller_->preparingCode()) return tr("the games are prepared for this version");
    return {};
}

void MainWindow::showUpdateDialog(bool required) {
    if (updateDialog_) {
        popUp(updateDialog_);
        return;
    }
    auto* dlg = new UpdateDialog(update_, required, Updater::blocker(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    updateDialog_ = dlg;
    connect(dlg, &UpdateDialog::updateNow, this, &MainWindow::startUpdate);
    connect(dlg, &UpdateDialog::cancelRequested, this, [this] {
        installPending_ = false;
        updater_->cancel();
    });
    connect(dlg, &QDialog::rejected, this, [this] {
        updateDismissed_ = update_.value(QStringLiteral("version")).toString();
        installPending_ = false;
        updater_->cancel();  // closed while it downloaded: nothing installed
    });
    popUp(dlg);
    QApplication::alert(this);
}

void MainWindow::startUpdate() {
    if (updater_->busy() || update_.isEmpty()) return;
    updater_->start(update_);
}

// The downloaded release goes in once nothing uses the games' files: no match, search or watched match under way and
// no single-player game open (the game waiting at its hold point for a match closes first). Then the new launcher
// starts and this one closes (Updater::installed).
void MainWindow::installUpdateWhenFree() {
    if (!updater_->busy()) {
        installPending_ = false;
        return;
    }
    if (const QString busy = gamesBusy(); !busy.isEmpty()) {
        installPending_ = true;
        if (updateDialog_) updateDialog_->setWaiting(tr("Downloaded. It goes in once %1.").arg(busy));
        return;
    }
    installPending_ = false;
    controller_->shutdown();
    updater_->install();
}

void MainWindow::showNotice(const QString& text, bool warn) {
    notice_->setText(text);
    notice_->setStyleSheet(warn ? QStringLiteral("background:#5A0911; color:#FFFFFF; padding:8px 24px; font-weight:600;")
                                : QStringLiteral("background:#1C1C22; color:#F0E6D8; padding:8px 24px; border-bottom:1px solid #8E0E1C;"));
    notice_->show();
    noticeTimer_.start(20000);
}

void MainWindow::cliLogin() {
    if (opts_.user.isEmpty() || server_->isLoggedIn() || cliLoginId_ >= 0) return;
    const QString pw = QProcessEnvironment::systemEnvironment().value(opts_.passwordEnv);
    if (pw.isEmpty()) {
        applog::write(QStringLiteral("cli: %1 is not set; not signing in").arg(opts_.passwordEnv));
        return;
    }
    cliTriedRegister_ = !opts_.registerAccount;
    cliLoginId_ = opts_.registerAccount ? server_->registerAccount(opts_.user, pw) : server_->login(opts_.user, pw);
    applog::write(QStringLiteral("cli: signing in as %1").arg(opts_.user));
}

void MainWindow::cliQueue() {
    if (opts_.queue.isEmpty() || cliQueued_ || !server_->isLoggedIn()) return;
    if (opts_.solo && !controller_->soloRunning()) {  // standby: single player first, then the search beside it
        QString err;
        if (!controller_->startSolo(gameId_, &err)) applog::write(QStringLiteral("cli: single player: %1").arg(err));
    }
    cliQueued_ = true;
    QueuePage* page = opts_.queue == QLatin1String("ranked") ? ranked_ : casual_;
    selectPage(opts_.queue == QLatin1String("ranked") ? kRanked : kCasual);
    applog::write(QStringLiteral("cli: queueing %1 %2 %3").arg(opts_.queue, gameId_, opts_.mode));
    page->startSearch(opts_.mode, opts_.character);
    if (opts_.cancelSearchSeconds >= 0)
        QTimer::singleShot(opts_.cancelSearchSeconds * 1000, this, [this] {
            applog::write(QStringLiteral("cli: cancelling the search"));
            controller_->cancelSearch();
        });
}

// --lobby host: create the lobby; --lobby join: look for it by name until it is there, then join; both mark themselves
// ready once two are in, and the host starts the match once both are ready (one match); --lobby watch: join it as a
// spectator, whose game plays the lobby's matches
void MainWindow::cliLobby() {
    if (opts_.lobby.isEmpty() || cliLobbyBegun_ || !server_->isLoggedIn()) return;
    cliLobbyBegun_ = true;
    selectPage(kLobby);
    const QString name = opts_.lobbyName.isEmpty() ? QStringLiteral("e2e lobby") : opts_.lobbyName;
    const QString mode = opts_.mode.isEmpty() ? QStringLiteral("kombat") : opts_.mode;
    if (opts_.lobby == QLatin1String("host")) {
        applog::write(QStringLiteral("cli: creating lobby \"%1\" for %2 %3").arg(name, gameId_, mode));
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_create")},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("game"), gameId_},
                       {QStringLiteral("mode"), mode},
                       {QStringLiteral("password"), QString()},
                       {QStringLiteral("rounds"), 3},
                       {QStringLiteral("region"), cfg::region()},
                       {QStringLiteral("character"), opts_.character},
                       {QStringLiteral("death_traps"), cfg::deathTraps()}});
        return;
    }
    applog::write(QStringLiteral("cli: looking for lobby \"%1\"").arg(name));
    cliLobbyPoll_ = new QTimer(this);
    connect(cliLobbyPoll_, &QTimer::timeout, this, [this] {
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_list")}, {QStringLiteral("game"), gameId_}});
    });
    cliLobbyPoll_->start(1000);
}

void MainWindow::cliLobbyMessage(const QJsonObject& msg) {
    const QString type = msg.value(QStringLiteral("type")).toString();
    const QString name = opts_.lobbyName.isEmpty() ? QStringLiteral("e2e lobby") : opts_.lobbyName;
    const bool watch = opts_.lobby == QLatin1String("watch");
    if (type == QLatin1String("lobbies") && (opts_.lobby == QLatin1String("join") || watch) && !cliLobbyJoined_) {
        for (const auto& v : msg.value(QStringLiteral("lobbies")).toArray()) {
            const QJsonObject lb = v.toObject();
            if (lb.value(QStringLiteral("name")).toString() != name ||
                (!watch && lb.value(QStringLiteral("state")).toString() != QLatin1String("open")))
                continue;
            cliLobbyJoined_ = true;
            if (cliLobbyPoll_) cliLobbyPoll_->stop();
            const QString code = lb.value(QStringLiteral("id")).toString();
            applog::write(QStringLiteral("cli: %1 lobby %2").arg(watch ? QStringLiteral("watching") : QStringLiteral("joining"), code));
            server_->send({{QStringLiteral("type"), QStringLiteral("lobby_join")},
                           {QStringLiteral("lobby"), code},
                           {QStringLiteral("password"), QString()},
                           {QStringLiteral("character"), opts_.character},
                           {QStringLiteral("spectate"), watch}});
            break;
        }
        return;
    }
    if (type != QLatin1String("lobby") || watch) return;  // a spectator never readies
    cliLastLobby_ = msg;
    const QJsonObject lobby = msg.value(QStringLiteral("lobby")).toObject();
    QJsonArray members;  // the players: spectators neither ready nor play
    for (const auto& v : lobby.value(QStringLiteral("members")).toArray())
        if (!v.toObject().value(QStringLiteral("spectator")).toBool()) members.append(v);
    if (lobby.value(QStringLiteral("state")).toString() == QLatin1String("in_match")) {
        cliLobbyInMatch_ = true;
        return;
    }
    if (cliLobbyInMatch_) {  // the lobby is open again: that match is over
        cliLobbyInMatch_ = false;
        cliLobbyDone_++;
        applog::write(QStringLiteral("cli: lobby match %1 of %2 over").arg(cliLobbyDone_).arg(opts_.lobbyMatches));
    }
    // --leave-lobby: out as soon as the last match is over, while the game may still play its end
    if (opts_.leaveLobby && cliLobbyDone_ >= opts_.lobbyMatches && !cliLobbyLeft_) {
        cliLobbyLeft_ = true;
        applog::write(QStringLiteral("cli: leaving lobby %1").arg(lobby.value(QStringLiteral("id")).toString()));
        controller_->stopWatching();
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_leave")}});
        return;
    }
    if (members.size() < 2 || cliLobbyDone_ >= opts_.lobbyMatches) return;
    if (controller_->phase() != MatchController::Phase::Idle) return;  // the game still plays the last match's end
    bool all_ready = true, me_ready = false;
    for (const auto& v : members) {
        const QJsonObject m = v.toObject();
        const bool ready = m.value(QStringLiteral("ready")).toBool();
        all_ready = all_ready && ready;
        if (m.value(QStringLiteral("name")).toString() == server_->accountName()) me_ready = ready;
    }
    if (!me_ready && cliLobbyReadyFor_ != cliLobbyDone_) {
        cliLobbyReadyFor_ = cliLobbyDone_;
        applog::write(QStringLiteral("cli: ready in lobby %1").arg(lobby.value(QStringLiteral("id")).toString()));
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_ready")}, {QStringLiteral("ready"), true},
                       {QStringLiteral("delay"), cfg::inputDelay()}, {QStringLiteral("relay"), cfg::hideIp()}});
    }
    // both ready: the server starts the match after its countdown
    if (opts_.lobby == QLatin1String("host") && all_ready && cliLobbyStarts_ == cliLobbyDone_) {
        cliLobbyStarts_++;
        applog::write(QStringLiteral("cli: both ready, the lobby's match %1 counts down").arg(cliLobbyStarts_));
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    controller_->shutdown();
    server_->disconnectFromServer();
    event->accept();
}
