#pragma once
// Ranked / Casual queue pages, Practice (with the local two-instance test) and the Leaderboard.

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

class PracticePage : public Page {
    Q_OBJECT
public:
    explicit PracticePage(MatchController* controller, QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;

private:
    void refreshTestRoster();
    MatchController* controller_;
    QComboBox* p1_;
    QComboBox* p2_;
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
