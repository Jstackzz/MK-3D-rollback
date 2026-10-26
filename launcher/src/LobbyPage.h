#pragma once
// Lobby browser and the current lobby: members, character, ready, host start, chat.

#include <QJsonObject>

#include "Widgets.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QStackedWidget;
class MatchController;
class ServerClient;

class LobbyPage : public Page {
    Q_OBJECT
public:
    LobbyPage(ServerClient* server, MatchController* controller, QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void onMessage(const QJsonObject& msg);
    void showLobby(const QJsonObject& lobby);
    void leftLobby();
    void join(const QString& code, bool locked);
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
    QTableWidget* members_;
    QComboBox* character_;
    QPushButton* ready_;
    QPushButton* start_;
    QPushButton* leave_;
    LogView* chat_;
    QLineEdit* chatInput_;
    QJsonObject lobby_;
};
