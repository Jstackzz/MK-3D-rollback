#pragma once
// Account sign-in/registration, the match-found accept prompt, and lobby creation.

#include <QDialog>
#include <QJsonObject>
#include <QTimer>

#include "GameData.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class ServerClient;

class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(ServerClient* server, const QString& lastName, QWidget* parent = nullptr);
    bool remember() const;
    QString name() const;
    void askForNewAccount();  // opened by itself on a PC with no account yet: a new one, and why it is asked

private:
    void setRegistering(bool on);
    void submit();
    void onMessage(const QJsonObject& msg);

    ServerClient* server_;
    QLabel* title_;
    QLabel* intro_;
    QLabel* error_;
    QLineEdit* name_;
    QLineEdit* password_;
    QLineEdit* confirm_;
    QLabel* confirmLabel_;
    QCheckBox* remember_;
    QPushButton* submit_;
    QPushButton* switch_;
    bool registering_ = false;
    int pendingId_ = -1;
};

class MatchFoundDialog : public QDialog {
    Q_OBJECT
public:
    MatchFoundDialog(const QJsonObject& found, const QString& gameTitle, const QString& modeName,
                     const QString& characterName, QWidget* parent = nullptr);
    QString matchId() const { return matchId_; }

private:
    QString matchId_;
    QProgressBar* bar_;
    QLabel* countdown_;
    QTimer tick_;
    qint64 deadlineMs_ = 0;
};

class CreateLobbyDialog : public QDialog {
    Q_OBJECT
public:
    CreateLobbyDialog(const Game& game, const QString& defaultName, QWidget* parent = nullptr);
    QString name() const;
    QString mode() const;
    QString password() const;
    int rounds() const;

private:
    QLineEdit* name_;
    QComboBox* mode_;
    QLineEdit* password_;
    QComboBox* rounds_;
};
