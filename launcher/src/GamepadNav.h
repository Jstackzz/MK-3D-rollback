#pragma once
// The launcher with a controller, through SDL3 as the games read theirs (XInput, Windows.Gaming.Input and HIDAPI:
// Xbox and PlayStation pads, fight sticks): while a launcher window is in front, the d-pad or the left stick moves the
// focus to the nearest control that way (or moves within a list or table first), A presses it, B backs out of a
// dialog (never the match-found or rematch prompt: a match should not go by a slip), LB / RB switch pages and START
// finds a match on a queue page. A window that comes up over a game ignores the buttons held from the game until they
// are let go. While a match is played (`paused`) the buttons are the game's alone, wherever the focus is: a launcher
// left in front by a game that could not take the focus must not act on a fight's presses.

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QTimer>

#include <functional>

class QWidget;
struct SDL_Gamepad;

class GamepadNav : public QObject {
    Q_OBJECT
public:
    explicit GamepadNav(QObject* parent = nullptr);
    ~GamepadNav() override;

    // what LB / RB (-1 / +1) and START do in the main window
    std::function<void(int)> onPage;
    std::function<void()> onStart;
    std::function<bool()> paused;
    // a dialog that B leaves alone (its own buttons decide)
    static void keepFromBack(QWidget* dialog);

private:
    enum Button { Up, Down, Left, Right, A, B, Start, PrevPage, NextPage, kButtons };
    void tick();
    void openPads();
    unsigned readButtons();
    void press(Button b);
    void move(int dx, int dy);
    void activate();

    QTimer timer_;
    QList<SDL_Gamepad*> pads_;
    QElapsedTimer sinceOpen_, sinceFront_, held_[kButtons];
    bool ready_ = false, front_ = false;
    unsigned raw_ = 0;      // every controller's buttons together, last tick
    unsigned last_ = 0;     // the ones that counted last tick
    unsigned latched_ = 0;  // held when a launcher window came to the front: they count once let go
    int repeats_[kButtons] = {};
};
