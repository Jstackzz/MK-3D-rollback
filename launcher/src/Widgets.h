#pragma once
// Reusable pieces: page base class, cards, the character grid, a timestamped log view.

#include <QFrame>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QWidget>

#include "GameData.h"

class QLabel;
class QVBoxLayout;
class QComboBox;

// A page of the main stack; told when the header's game selector changes.
class Page : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    virtual void setGame(const QString& gameId) { gameId_ = gameId; }
    QString gameId() const { return gameId_; }

protected:
    QString gameId_ = QStringLiteral("mkd");
};

namespace ui {
QFrame* card(const QString& title, QVBoxLayout** body, QWidget* parent = nullptr);
QLabel* label(const QString& text, const char* objectName = nullptr, QWidget* parent = nullptr);
QWidget* pageHeader(const QString& title, const QString& subtitle, QLabel** subtitleOut = nullptr);
void letterSpacing(QWidget* w, qreal pixels);
void fillRegions(QComboBox* box, const QString& select);
void fillMaxPing(QComboBox* box, int select);
QWidget* statTile(const QString& caption, QLabel** valueOut);
}  // namespace ui

class CharacterPicker : public QListWidget {
    Q_OBJECT
public:
    explicit CharacterPicker(QWidget* parent = nullptr);
    void setRoster(const QList<Character>& roster, const QString& select = {});
    QString currentId() const;
    QString currentName() const;
    void selectId(const QString& id);

signals:
    void picked(const QString& id);
};

class LogView : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit LogView(QWidget* parent = nullptr);
    void line(const QString& text);
};
