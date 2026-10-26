#pragma once
// Controls: which keys and which controller buttons press what in the games (Controls.h), saved at each change.

#include <array>

#include "Controls.h"
#include "Widgets.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

class ControlsPage : public Page {
    Q_OBJECT
public:
    explicit ControlsPage(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void pickKey(int control, int slot);
    void showMap();  // the widgets from map_
    void store();    // map_ into the file the games read
    void say(const QString& text, bool warn = false);

    controls::Map map_;
    QCheckBox* keyboard_ = nullptr;
    std::array<std::array<QPushButton*, controls::kKeysPerButton>, controls::Count> keyButtons_{};
    QList<QComboBox*> pressBoxes_;
    QLabel* unpressed_ = nullptr;
    QLabel* status_ = nullptr;
};
