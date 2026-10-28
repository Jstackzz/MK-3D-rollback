#pragma once
// Lobby browser and the current lobby: members, character, ready (both players ready starts the match after a 3-2-1
// countdown), chat, and spectators: members who watch the lobby's matches in their own game instead of playing them
// (docs/PROTOCOL.md 1.10.1).

#include <QJsonObject>

#include "Widgets.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QStackedWidget;
class QTimer;
class MatchController;
class ServerClient;

class LobbyPage : public Page {
    Q_OBJECT
public:
    LobbyPage(ServerClient* server, MatchController* controller, QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;
    void refresh();
    bool inLobby() const { return !lobby_.isEmpty(); }
    // a controller's START on this page presses Ready (a player's, not a spectator's); after a lobby match the focus
    // goes back to it, so A readies up for the next one
    void pressReady();
    void focusReady();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void onMessage(const QJsonObject& msg);
    void showLobby(const QJsonObject& lobby);
    void updateStatus();  // the countdown, MATCH IN PROGRESS, or how the next match starts
    void leftLobby(bool keepWatching = false);
    void join(const QString& code, bool locked, bool spectate = false);
    bool spectating() const;  // this player watches in the lobby they are in
    void createLobby();
    void sendChat();
    void appendChat(const QString& from, const QString& text, double ts);
    QString savedCharacter(const QString& game, const QString& mode) const;

    ServerClient* server_;
    MatchController* controller_;
    QTableWidget* list_;
    QLineEdit* code_;
    QLabel* listInfo_;
    QStackedWidget* right_;
    QLabel* lobbyTitle_;
    QLabel* lobbyInfo_;
    QCheckBox* deathTraps_;  // the lobby's: the host changes it between matches (lobby_settings), the rest see it
    QTableWidget* members_;
    QComboBox* character_;
    QPushButton* ready_;
    QLabel* countdown_;  // STARTING IN 3, 2, 1 / MATCH IN PROGRESS
    QLabel* nextInfo_;   // how the next match starts, when nothing counts down
    QTimer* tick_;
    qint64 startsAtMs_ = 0;  // when the match starts by itself (the lobby state's starts_in), 0: no countdown
    QPushButton* leave_;
    QPushButton* role_;  // PLAY (a spectator takes a free place) / WATCH (a player steps aside)
    LogView* chat_;
    QLineEdit* chatInput_;
    QJsonObject lobby_;
    QTimer* rejoinGrace_;  // a spectator's dropped connection: back in the lobby within this, or the watching stops
    bool watchingHere_ = false;  // a spectator in the lobby shown (kept: the account's name is gone with the connection)
};
