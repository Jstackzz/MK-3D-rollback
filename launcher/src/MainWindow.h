#pragma once
// The launcher window: header (title, game selector, account), navigation, pages, footer.

#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

class QButtonGroup;
class QLabel;
class QPushButton;
class QStackedWidget;
class GamepadNav;
class LobbyPage;
class MatchController;
class MatchFoundDialog;
class RematchDialog;
class Page;
class QueuePage;
class BuildPage;
class SinglePlayerPage;
class ServerClient;
class SettingsPage;
class UpdateDialog;
class Updater;

struct LaunchOptions {
    QString server;
    QString fingerprint;
    QString user;
    QString passwordEnv = QStringLiteral("MK3D_DEV_PASSWORD");
    bool registerAccount = false;
    QString queue;  // "casual" or "ranked"
    QString game;
    QString mode;
    QString character;
    QString page;
    int autoAcceptSeconds = -1;
    bool localTest = false;  // start the two-instance loopback test right away
    QString extractImage;    // open "Build game" and extract this disc image right away
    QString lobby;           // "host": create a lobby and start it once both are ready; "join": join it by name
    QString lobbyName;
    int lobbyMatches = 1;    // matches to play there, one after the other
    int rematches = 0;       // --queue casual: rematches to choose after the first match
    QStringList rematchChoices;  // what each of them chooses, "rematch" or "select" (the last one repeats)
    bool leaveSeries = false;    // once those are chosen, leave the series at once instead of letting it run out
    bool solo = false;           // --queue: single player first, the search running alongside it (standby)
    QString practice;            // "on" / "off": practice while a search waits, saved (cfg::practiceWhileWaiting)
    QString deathTraps;          // "on" / "off": the arenas' death traps, saved (cfg::deathTraps)
    bool upnpProbe = false;  // find the router and its public address, open nothing
    QString updatedFrom;     // --updated: the version this launcher just replaced (Updater)
    bool autoUpdate = false; // development: an update offered installs at once, without asking
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const LaunchOptions& opts, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QWidget* buildHeader();
    QWidget* buildNav();
    QWidget* buildFooter();
    void selectGame(const QString& id);
    void selectPage(int index);
    void connectServer();
    void updateFooter();
    void updateAccountButton();
    void onAccountButton();
    void openAccountDialog(bool firstTime);
    void askForAccount();
    void warnSmartAppControl();  // it blocks the games: what it is and where to switch it off
    void onServerMessage(const QJsonObject& msg);
    void onMatchFound(const QJsonObject& found);
    // the Kasual series card's state: the rematch prompt over the game once its match's end has played
    void onKasualSeries(const QJsonObject& series, const QString& title, const QString& score, const QString& info,
                        bool canChoose, bool canSelect);
    void popUp(QWidget* dialog);  // in front of the game, the launcher's window restored first when minimized
    void focusGame();             // the game's window in front again: the player is back in it
    void frontSoloWhenUp();       // single player's (practice's) new game window in front once it has one
    void showNotice(const QString& text, bool warn);
    // a release newer than this launcher (welcome, an update push, or the reply turning away a launcher too old)
    void offerUpdate(const QJsonValue& release, bool required);
    void showUpdateDialog(bool required);
    void startUpdate();
    void installUpdateWhenFree();
    // what keeps the games in use (an update waits for it: no window over a match or a game, no install under one),
    // as the end of "once ..."; empty when nothing does
    QString gamesBusy() const;
    void cliLogin();
    void cliQueue();
    void cliLobby();
    void cliLobbyMessage(const QJsonObject& msg);

    LaunchOptions opts_;
    ServerClient* server_;
    MatchController* controller_;
    QStackedWidget* stack_ = nullptr;
    QList<Page*> pages_;
    QueuePage* ranked_ = nullptr;
    QueuePage* casual_ = nullptr;
    SettingsPage* settings_ = nullptr;
    class UpnpMapper* upnp_ = nullptr;
    BuildPage* build_ = nullptr;
    QButtonGroup* navGroup_ = nullptr;
    QButtonGroup* gameGroup_ = nullptr;
    QPushButton* account_ = nullptr;
    QLabel* notice_ = nullptr;
    QLabel* connDot_ = nullptr;
    QLabel* connText_ = nullptr;
    QLabel* onlineText_ = nullptr;
    QLabel* gameText_ = nullptr;
    QLabel* phaseText_ = nullptr;
    QTimer noticeTimer_;
    GamepadNav* pad_ = nullptr;  // the launcher with a controller
    QTimer soloFrontTimer_;  // frontSoloWhenUp's tries, 30 s at most
    int soloFrontTries_ = 0;
    QString gameId_;
    QString lastError_;
    QJsonObject online_;
    QPointer<MatchFoundDialog> matchDialog_;
    QPointer<RematchDialog> rematchDialog_;
    Updater* updater_ = nullptr;
    QPointer<UpdateDialog> updateDialog_;
    QPushButton* updateButton_ = nullptr;  // the footer's, once a release is offered (LATER leaves it)
    QJsonObject update_;                   // the release offered
    QString updateDismissed_;              // the version LATER was chosen for this run
    bool installPending_ = false;          // downloaded, waiting for the match or the game to be over
    bool offerPending_ = false;            // offered during a match or a game: its window comes once that is over
    bool offerRequired_ = false;
    QString rematchShownFor_;  // the series and matches played the prompt last came up for (once each)
    bool askedForAccount_ = false;  // the account window opened by itself this run
    int cliLoginId_ = -1;
    bool cliTriedRegister_ = false;
    bool cliQueued_ = false;
    int cliRematches_ = 0;  // rematches chosen
    bool cliLobbyBegun_ = false, cliLobbyJoined_ = false, cliLobbyInMatch_ = false;
    int cliLobbyDone_ = 0, cliLobbyReadyFor_ = -1, cliLobbyStarts_ = 0;  // matches over, ready sent for, starts sent
    QTimer* cliLobbyPoll_ = nullptr;
    QJsonObject cliLastLobby_;  // the lobby's last state, looked at again once the controller is idle
    bool darkApplied_ = false;
};
