#pragma once
// The controls file the games read (the runtime's core/input.cpp): which keys press each button of the original Xbox
// controller, and what each button of a PC controller presses. The Controls page edits it; a game that is running
// reads it again within a second, and the launcher hands its path to every game it starts (XBE_CONTROLS).

#include <QList>
#include <QString>

#include <array>

class QKeyEvent;

namespace controls {

// the original Xbox controller's buttons, in the order the page lists them
enum Control { Up, Down, Left, Right, A, B, X, Y, Black, White, LeftTrigger, RightTrigger, Start, Back, LeftThumb,
               RightThumb, Count };
QString controlId(int control);     // the file's name: "up", "a", "left_trigger"
QString controlLabel(int control);  // "D-pad up", "A", "Left trigger"
int controlFromId(const QString& id);

// a PC controller's button, named by where it sits (the bottom face button is Xbox A, PlayStation cross, Switch B)
struct Source {
    QString id;     // the file's name: "south", "left_shoulder", "paddle1"
    QString label;  // "Bottom face button (A / Cross)"
    QString hint;   // what it is called on each kind of controller
    int control;    // what it presses unless changed, -1 nothing
};
const QList<Source>& sources();

constexpr int kKeysPerButton = 2;

struct Map {
    bool keyboard = true;                      // the keyboard plays as player 1
    std::array<QList<int>, Count> keys;        // Windows virtual keys, at most kKeysPerButton each
    QList<int> presses;                        // per sources() entry: the control it presses, -1 nothing
};
Map defaults();
QString file();  // controls.toml beside the launcher's settings file
Map load();      // the file's controls over the defaults
bool save(const Map& map, QString* error);

QString keyName(int vk);  // as the file writes it: "K", "Space", "LShift", a code the names miss as "0xE5"
int keyFromName(const QString& name);
QString keyLabel(int vk);  // for people: "K", "Left Shift", the sign this keyboard's layout puts on the key
// the virtual key of a press, left and right Shift and Ctrl apart; 0 for a key a game cannot use (Alt and F10 open
// the window menu, the Windows keys and Esc belong to Windows and to leaving, F3 is a game's diagnostics key)
int keyFromEvent(const QKeyEvent* event);

}  // namespace controls
