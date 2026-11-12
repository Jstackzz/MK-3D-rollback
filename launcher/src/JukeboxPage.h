#pragma once
// The Jukebox page: a soundtrack of another Mortal Kombat for the game's stages and select screen, or a song per
// stage, from the music folder (Jukebox.h). Changes apply by themselves a moment after the last one.

#include <QList>
#include <QMap>
#include <QTimer>

#include "Jukebox.h"
#include "Widgets.h"

class QComboBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QStandardItemModel;
class QWidget;

class JukeboxPage : public Page {
    Q_OBJECT
public:
    explicit JukeboxPage(QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    struct Row {
        int stage = -1;  // -1: the select screen
        QComboBox* choice = nullptr;
        QLabel* plays = nullptr;
        QPushButton* play = nullptr;
    };
    void rescan();
    void rebuildTrackModel();
    void rebuildRows();
    void showConfig();     // the widgets from config_
    void refreshPlan();    // plays labels and notes from the plan
    void changed();        // a widget changed: save, plan, apply soon
    void applyNow();
    void preview(const Row& row);
    QString choiceOf(QComboBox* box) const;
    void selectChoice(QComboBox* box, const QString& choice);

    QList<jukebox::Soundtrack> library_;
    jukebox::Config config_;
    jukebox::Plan plan_;
    jukebox::Applier applier_;
    jukebox::Preview preview_;
    QTimer applyTimer_;
    bool applyAgain_ = false;
    bool loading_ = false;

    QStandardItemModel* tracks_ = nullptr;
    QLabel* folder_ = nullptr;
    QLabel* libraryInfo_ = nullptr;
    QLabel* title_ = nullptr;
    QComboBox* soundtrack_ = nullptr;
    QLabel* soundtrackInfo_ = nullptr;
    Row selectRow_;
    QGridLayout* stageGrid_ = nullptr;
    QWidget* stageBox_ = nullptr;
    QList<Row> rows_;
    QLabel* notes_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* apply_ = nullptr;
};
