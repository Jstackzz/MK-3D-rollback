#pragma once
// Ranked / Kasual queue pages, Single player (with the local two-instance test) and the Leaderboard.

#include <QElapsedTimer>
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
    // the series card's choices, the rematch prompt's too: "rematch" or "select", or leaving the series
    void choose(const QString& choice);
    void leaveSeries();
    void pressFind();  // FIND MATCH (or cancelling the search) as a click would, for a controller's START

signals:
    // the series card's state at every change (series {}: none any more), mirrored by the rematch prompt
    void seriesCard(const QJsonObject& series, const QString& title, const QString& score, const QString& info,
                    bool canChoose, bool canSelect);

protected:
    void showEvent(QShowEvent* event) override;  // the other queue page may have changed the shared settings

private:
    void refreshModes();
    void refreshRoster();
    void updateStats();
    void updatePlayers();
    void updateButton();
    void onPhase(MatchController::Phase phase, const QString& text);
    // the series after a match: a Kasual rematch, or the ranked set's score and its next match
    void onSeries(const QJsonObject& series);
    void updateSeries();
    QString seriesOverText(const QJsonObject& series) const;
    bool leavingLosesSet();  // asks first when leaving would lose a ranked set; true: go ahead
    QString key(const QString& what) const;

    bool ranked_;
    ServerClient* server_;
    MatchController* controller_;
    QComboBox* mode_;
    QLabel* modeInfo_;
    CharacterPicker* picker_;
    QLabel* inGamePick_;
    QCheckBox* pickInGame_;  // pick on the game's own select screen (modes the launcher picks for otherwise)
    QLabel* playersNow_;   // how many play this game right now (the server's counts)
    QLabel* playersInfo_;  // searching (for this page's queue too), in matches, in lobbies
    QComboBox* region_;
    QComboBox* maxPing_;
    QCheckBox* strict_;
    QCheckBox* practice_;  // practice while the search waits (shared by both queue pages)
    QCheckBox* deathTraps_;  // the arenas' death traps (both queue pages)
    QLabel* delayInfo_;
    QPushButton* find_;
    QLabel* status_;
    QLabel* timer_;
    QLabel* recordTitle_;
    QLabel* elo_;
    QLabel* wins_;
    QLabel* losses_;
    QWidget* mmCard_;
    QWidget* seriesCard_;
    QLabel* seriesTitle_;
    QLabel* seriesScore_;
    QLabel* seriesInfo_;
    QPushButton* rematch_;  // Kasual: the same fighters again
    QPushButton* select_;   // Kasual: both back to character select
    QPushButton* leave_;
    LogView* log_;
    QTimer clock_;
    QTimer seriesClock_;
    QElapsedTimer seriesSince_;  // since the series' last message, for its countdown
    QJsonObject stats_;
    QJsonObject online_;      // the server's last online counts
    QJsonObject series_;      // this page's series (Kasual or ranked), {} when none
    QString chosen_;  // "rematch" or "select" pressed, not seen by the server yet (sent once the game is ready)
};

// the whole game with its own menus (arcade, its practice mode, the side modes), straight to practice with every
// fighter unlocked, and in a source build the two-game local test
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
    QPushButton* practice_;
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
