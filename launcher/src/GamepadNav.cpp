#include "GamepadNav.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTextEdit>

#include <SDL3/SDL.h>

#include <algorithm>

#include <windows.h>

namespace {

constexpr int kTickMs = 16;
constexpr qint64 kQuietMs = 250;                         // after a launcher window came to the front
constexpr qint64 kRepeatDelayMs = 380, kRepeatMs = 110;  // a direction held
constexpr int kStickOn = 20000, kStickOff = 12000;       // the left stick as directions (pushed, let go)

// the foreground window is one of this launcher's: what the controllers do is meant for it (the games read them too,
// in the background, and the launcher must not act on what a player does in a game)
bool LauncherInFront() {
    if (!QApplication::activeWindow()) return false;
    static const bool anyWindow = qEnvironmentVariableIsSet("MK3D_PAD_ANY_WINDOW");  // tests on Qt's offscreen platform
    if (anyWindow) return true;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// what the focus can go to: the controls, not the logs and texts to read
bool Navigable(QWidget* w) {
    if (!w->isVisible() || !w->isEnabled() || !(w->focusPolicy() & Qt::TabFocus)) return false;
    if (qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QTextEdit*>(w)) return false;
    return qobject_cast<QAbstractButton*>(w) || qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractItemView*>(w) ||
           qobject_cast<QLineEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QAbstractSlider*>(w);
}

void SendKey(QWidget* to, int key) {
    QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier), up(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(to, &down);
    QApplication::sendEvent(to, &up);
}

QRect InWindow(QWidget* w, QWidget* window) { return QRect(w->mapTo(window, QPoint(0, 0)), w->size()); }

}  // namespace

GamepadNav::GamepadNav(QObject* parent) : QObject(parent) {
    // the games' settings (runtime core/input.cpp): XInput devices as gamepads, PlayStation and Switch pads through
    // HIDAPI, the rest through Windows.Gaming.Input, read in the background as well
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    ready_ = SDL_Init(SDL_INIT_GAMEPAD);
    if (!ready_) return;
    openPads();
    sinceOpen_.start();
    timer_.setInterval(kTickMs);
    connect(&timer_, &QTimer::timeout, this, &GamepadNav::tick);
    timer_.start();
}

GamepadNav::~GamepadNav() {
    for (SDL_Gamepad* p : pads_) SDL_CloseGamepad(p);
    if (ready_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void GamepadNav::keepFromBack(QWidget* dialog) { dialog->setProperty("padKeepsFromBack", true); }

// the controllers plugged in now (checked once a second): new ones opened, gone ones closed
void GamepadNav::openPads() {
    for (qsizetype i = pads_.size() - 1; i >= 0; i--)
        if (!SDL_GamepadConnected(pads_[i])) {
            SDL_CloseGamepad(pads_[i]);
            pads_.removeAt(i);
        }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) {
        const bool open = std::any_of(pads_.cbegin(), pads_.cend(), [&](SDL_Gamepad* p) { return SDL_GetGamepadID(p) == ids[i]; });
        if (!open)
            if (SDL_Gamepad* p = SDL_OpenGamepad(ids[i])) pads_.append(p);
    }
    SDL_free(ids);
}

// every controller's buttons together (any of them steers the launcher)
unsigned GamepadNav::readButtons() {
    SDL_UpdateGamepads();
    unsigned out = 0;
    auto set = [&out](Button b, bool on) {
        if (on) out |= 1u << b;
    };
    for (SDL_Gamepad* p : pads_) {
        auto on = [p](SDL_GamepadButton b) { return SDL_GetGamepadButton(p, b); };
        // the stick past kStickOn turns a direction on, back under kStickOff off
        auto stick = [this](int value, Button b) { return value > ((raw_ & (1u << b)) ? kStickOff : kStickOn); };
        const int x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
        set(Up, on(SDL_GAMEPAD_BUTTON_DPAD_UP) || stick(-y, Up));
        set(Down, on(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || stick(y, Down));
        set(Left, on(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || stick(-x, Left));
        set(Right, on(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || stick(x, Right));
        set(A, on(SDL_GAMEPAD_BUTTON_SOUTH));  // A, Cross
        set(B, on(SDL_GAMEPAD_BUTTON_EAST));   // B, Circle
        set(Start, on(SDL_GAMEPAD_BUTTON_START));
        set(PrevPage, on(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
        set(NextPage, on(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    }
    return out;
}

void GamepadNav::tick() {
    if (sinceOpen_.elapsed() > 1000) {
        openPads();
        sinceOpen_.restart();
    }
    const unsigned now = readButtons();
    raw_ = now;
    const bool front = LauncherInFront() && !(paused && paused());
    if (front && !front_) sinceFront_.start();  // a launcher window came up, over a game maybe
    front_ = front;
    if (!front) {
        last_ = 0;
        latched_ = now;  // held from the game: counts once let go
        return;
    }
    if (sinceFront_.elapsed() < kQuietMs) latched_ |= now;
    latched_ &= now;
    const unsigned active = now & ~latched_;
    for (int i = 0; i < kButtons; i++) {
        const unsigned bit = 1u << i;
        if ((active & bit) && !(last_ & bit)) {
            held_[i].start();
            repeats_[i] = 0;
            press(Button(i));
        } else if ((active & bit) && i <= Right) {  // a direction held repeats
            const qint64 t = held_[i].elapsed();
            const int due = t < kRepeatDelayMs ? 0 : 1 + int((t - kRepeatDelayMs) / kRepeatMs);
            if (due > repeats_[i]) {
                repeats_[i] = due;
                press(Button(i));
            }
        }
    }
    last_ = active;
}

void GamepadNav::press(Button b) {
    QWidget* window = QApplication::activeWindow();
    // an open list (a combo box's): arrows, Enter and Esc as on the keyboard
    if (QWidget* popup = QApplication::activePopupWidget()) {
        static const int keys[kButtons] = {Qt::Key_Up, Qt::Key_Down, 0, 0, Qt::Key_Return, Qt::Key_Escape, 0, 0, 0};
        if (keys[b]) SendKey(popup->focusWidget() ? popup->focusWidget() : popup, keys[b]);
        return;
    }
    if (!window) return;
    const bool dialog = qobject_cast<QDialog*>(window) != nullptr;
    switch (b) {
        case Up: move(0, -1); break;
        case Down: move(0, 1); break;
        case Left: move(-1, 0); break;
        case Right: move(1, 0); break;
        case A: activate(); break;
        case B:
            if (dialog && !window->property("padKeepsFromBack").toBool()) SendKey(window, Qt::Key_Escape);
            break;
        case Start:
            if (!dialog && onStart) onStart();
            break;
        case PrevPage:
        case NextPage:
            if (!dialog && onPage) onPage(b == PrevPage ? -1 : 1);
            break;
        default: break;
    }
}

// the focus to the nearest control that way: along the direction edge to edge, off to the side counting three times
// (0 when they overlap that way); a list or table moves its own current item first, as far as its edge
void GamepadNav::move(int dx, int dy) {
    QWidget* window = QApplication::activeWindow();
    QWidget* from = QApplication::focusWidget();
    if (from && (from->window() != window || !Navigable(from))) from = nullptr;
    if (auto* view = qobject_cast<QAbstractItemView*>(from)) {
        const QModelIndex before = view->currentIndex();
        SendKey(view, dx < 0 ? Qt::Key_Left : dx > 0 ? Qt::Key_Right : dy < 0 ? Qt::Key_Up : Qt::Key_Down);
        if (before.isValid() && view->currentIndex() != before) return;
    }
    QWidget* best = nullptr;
    double bestScore = 0;
    const QList<QWidget*> all = window->findChildren<QWidget*>();
    if (!from) {  // nothing focused yet: the window's default button, or the first control from the top left
        for (QWidget* w : all)
            if (auto* b = qobject_cast<QPushButton*>(w); b && b->isDefault() && Navigable(b)) best = b;
        if (!best)
            for (QWidget* w : all) {
                if (!Navigable(w)) continue;
                const QPoint c = InWindow(w, window).center();
                const double score = c.y() * 4.0 + c.x();
                if (!best || score < bestScore) {
                    best = w;
                    bestScore = score;
                }
            }
    } else {
        const QRect r0 = InWindow(from, window);
        QWidget* candidate = nullptr;
        for (QWidget* w : all) {
            if (w == from || !Navigable(w) || w->isAncestorOf(from) || from->isAncestorOf(w)) continue;
            const QRect r = InWindow(w, window);
            int along, side;
            if (dx) {
                along = dx > 0 ? r.left() - r0.right() : r0.left() - r.right();
                side = std::max({0, r.top() - r0.bottom(), r0.top() - r.bottom()});
            } else {
                along = dy > 0 ? r.top() - r0.bottom() : r0.top() - r.bottom();
                side = std::max({0, r.left() - r0.right(), r0.left() - r.right()});
            }
            if (along < -4) continue;  // not that way
            const double score = std::max(0, along) + side * 3.0;
            if (!candidate || score < bestScore) {
                candidate = w;
                bestScore = score;
            }
        }
        best = candidate;
    }
    if (!best) return;
    best->setFocus(Qt::TabFocusReason);
    for (QWidget* p = best->parentWidget(); p; p = p->parentWidget())
        if (auto* area = qobject_cast<QScrollArea*>(p)) area->ensureWidgetVisible(best, 24, 24);
}

// A: the focused control as a click would (a combo box opens its list); with none, the focus comes first
void GamepadNav::activate() {
    QWidget* w = QApplication::focusWidget();
    if (!w || w->window() != QApplication::activeWindow() || !Navigable(w)) {
        move(0, 1);
        return;
    }
    if (auto* b = qobject_cast<QAbstractButton*>(w)) b->animateClick();
    else if (auto* c = qobject_cast<QComboBox*>(w)) c->showPopup();
    else SendKey(w, Qt::Key_Return);
}
