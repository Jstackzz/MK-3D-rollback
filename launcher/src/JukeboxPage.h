#pragma once
// The Jukebox page, as Tekken 8's: a row of music sets (the game's own music, each soundtrack as its preset or
// shuffled, every soundtrack shuffled, three playlists of the player's own) with what the chosen one plays where; a
// playlist opens its own screen: each stage and menu with its song on the left, the soundtracks' songs game by game on
// the right, to hear and to pick. Changes apply by themselves a moment after the last one.

#include <QElapsedTimer>
#include <QList>
#include <QTimer>

#include "Jukebox.h"
#include "Widgets.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;

class JukeboxPage : public Page {
    Q_OBJECT
public:
    explicit JukeboxPage(QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void rescan();
    void fillSets(bool toActive);  // the music set tiles; the current one the set that plays, or the one that was
    void fillSlots();              // the stages and screens, in the overview and in the editor
    void fillTabs();               // a tab a soundtrack, and the game's own
    void fillTracks();             // the songs of the tab's soundtrack
    void refresh();                // after a change of set or song: the plan, the hints, the second lines and the checks
    void setClicked(QListWidgetItem* item);
    void chooseSet(const QString& set);
    void openEditor(const QString& slot);  // the playlist's screen, that stage or menu current
    void closeEditor();
    void assign(const QString& choice);  // the current slot of the playlist gets it ("own": the game's own)
    void trackClicked(QListWidgetItem* item, bool fromMouse);
    void changed();  // save, plan, apply in a moment
    void togglePreview(const QString& file);
    QString currentSlot() const;
    QString tabSoundtrack() const;  // the open tab's soundtrack id; empty for the game's own
    void showSlotSong();            // the tab of the current slot's song, that song current
    void copyInto(int playlist);

    QList<jukebox::Soundtrack> library_;
    jukebox::Config config_;
    jukebox::Plan plan_;
    jukebox::Preview preview_;
    QTimer applyTimer_;
    QElapsedTimer lastClick_;
    bool loading_ = false;

    QStackedWidget* views_ = nullptr;
    // the sets
    QListWidget* sets_ = nullptr;
    QLabel* setHint_ = nullptr;
    QLabel* overviewTitle_ = nullptr;
    QListWidget* overview_ = nullptr;
    QPushButton* edit_ = nullptr;
    QPushButton* copy_ = nullptr;
    // a playlist's screen
    QLabel* editorTitle_ = nullptr;
    QListWidget* slots_ = nullptr;
    QListWidget* tabs_ = nullptr;
    QLabel* tabTitle_ = nullptr;
    QListWidget* tracks_ = nullptr;
    QPushButton* play_ = nullptr;
    QPushButton* reset_ = nullptr;
    QPushButton* resetAll_ = nullptr;
    QPushButton* fill_ = nullptr;
    // both
    QLabel* folder_ = nullptr;
    QLabel* notes_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* apply_ = nullptr;
};
