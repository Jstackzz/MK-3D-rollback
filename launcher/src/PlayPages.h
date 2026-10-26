#pragma once
// Ranked / Casual queue pages, Single player (with the local two-instance test) and the Leaderboard.

#include <QJsonObject>
#include <QTimer>

#include "MatchController.h"
#include "Widgets.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class ServerClient;

class QueuePage : public Page {
    Q_OBJECT
public:
    QueuePage(bool ranked, ServerClient* server, MatchController* controller, QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;
    bool startSearch(const QString& mode = {}, const QString& character = {});  // also used by --queue

private:
    void refreshModes();
    void refreshRoster();
    void updateStats();
    void updateButton();
    void onPhase(MatchController::Phase phase, const QString& text);
    QString key(const QString& what) const;

    bool ranked_;
    ServerClient* server_;
    MatchController* controller_;
    QComboBox* mode_;
    QLabel* modeInfo_;
    CharacterPicker* picker_;
    QLabel* inGamePick_;
    QCheckBox* pickInGame_;  // pick on the game's own select screen (modes the launcher picks for otherwise)
    QComboBox* region_;
    QComboBox* maxPing_;
    QCheckBox* strict_;
    QLabel* delayInfo_;
    QPushButton* find_;
    QLabel* status_;
    QLabel* timer_;
    QLabel* recordTitle_;
    QLabel* elo_;
    QLabel* wins_;
    QLabel* losses_;
    LogView* log_;
    QTimer clock_;
    QJsonObject stats_;
};

// the whole game with its own menus (arcade, its practice mode, the side modes), and in a source build the two-game
// local test
class SinglePlayerPage : public Page {
    Q_OBJECT
public:
    explicit SinglePlayerPage(MatchController* controller, QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;

private:
    void refreshTestRoster();
    void updateButtons();
    MatchController* controller_;
    QLabel* title_;
    QPushButton* play_;
    QPushButton* close_;
    QComboBox* testMode_;
    QComboBox* testP1_;
    QComboBox* testP2_;
    QLabel* exeInfo_;
    QLabel* status_;
    LogView* log_;
};

class LeaderboardPage : public Page {
    Q_OBJECT
public:
    explicit LeaderboardPage(ServerClient* server, QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;
    void refresh();

private:
    void onMessage(const QJsonObject& msg);
    ServerClient* server_;
    QComboBox* mode_;
    QTableWidget* table_;
    QLabel* me_;
    QLabel* info_;
};
