#include "Dialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "AppPaths.h"
#include "MatchController.h"
#include "ServerClient.h"
#include "Style.h"
#include "Widgets.h"
#include "WinUtil.h"

// --- LoginDialog ----------------------------------------------------------------------------------

LoginDialog::LoginDialog(ServerClient* server, const QString& lastName, QWidget* parent)
    : QDialog(parent), server_(server) {
    setWindowTitle(tr("MK3D Online account"));
    setModal(true);
    setMinimumWidth(420);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 20, 24, 20);
    v->setSpacing(12);
    title_ = ui::label(QString(), "PageTitle");
    ui::letterSpacing(title_, 3);
    v->addWidget(title_);
    intro_ = new QLabel;
    intro_->setWordWrap(true);
    intro_->hide();
    v->addWidget(intro_);
    auto* hint = ui::label(tr("Names are 3-16 letters, digits, '_' or '-'. Passwords have at least 6 characters."), "Muted");
    hint->setWordWrap(true);
    v->addWidget(hint);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    name_ = new QLineEdit(lastName);
    name_->setMaxLength(16);
    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    confirm_ = new QLineEdit;
    confirm_->setEchoMode(QLineEdit::Password);
    confirmLabel_ = new QLabel(tr("Repeat password"));
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Password"), password_);
    form->addRow(confirmLabel_, confirm_);
    v->addLayout(form);
    remember_ = new QCheckBox(tr("Remember me on this PC"));
    remember_->setChecked(true);
    v->addWidget(remember_);
    error_ = ui::label(QString(), "Warning");
    error_->setWordWrap(true);
    v->addWidget(error_);

    auto* buttons = new QHBoxLayout;
    switch_ = new QPushButton;
    switch_->setFlat(true);
    submit_ = new QPushButton;
    submit_->setDefault(true);
    buttons->addWidget(switch_);
    buttons->addStretch();
    buttons->addWidget(submit_);
    v->addLayout(buttons);

    connect(switch_, &QPushButton::clicked, this, [this] { setRegistering(!registering_); });
    connect(submit_, &QPushButton::clicked, this, &LoginDialog::submit);
    connect(server_, &ServerClient::message, this, &LoginDialog::onMessage);
    setRegistering(false);
    if (!lastName.isEmpty()) password_->setFocus();
}

bool LoginDialog::remember() const { return remember_->isChecked(); }
QString LoginDialog::name() const { return name_->text().trimmed(); }

void LoginDialog::askForNewAccount() {
    setRegistering(true);
    intro_->setText(tr("Welcome! To play online you need an account on this server: choose a name and a password. "
                       "Have one already? Choose \"I already have an account\". You can also do this later with "
                       "SIGN IN at the top right."));
    intro_->show();
    name_->setFocus();
}

void LoginDialog::setRegistering(bool on) {
    registering_ = on;
    title_->setText(on ? tr("CREATE ACCOUNT") : tr("SIGN IN"));
    submit_->setText(on ? tr("Create account") : tr("Sign in"));
    switch_->setText(on ? tr("I already have an account") : tr("Create a new account"));
    confirm_->setVisible(on);
    confirmLabel_->setVisible(on);
    if (!on) intro_->hide();  // the welcome is about making one
    error_->clear();
}

void LoginDialog::submit() {
    static const QRegularExpression nameRe(QStringLiteral("^[A-Za-z0-9_-]{3,16}$"));
    const QString n = name(), p = password_->text();
    if (!nameRe.match(n).hasMatch()) {
        error_->setText(tr("Names are 3-16 letters, digits, '_' or '-'."));
        return;
    }
    if (p.size() < 6 || p.size() > 128) {
        error_->setText(tr("Passwords are 6-128 characters."));
        return;
    }
    if (registering_ && p != confirm_->text()) {
        error_->setText(tr("The two passwords differ."));
        return;
    }
    if (!server_->isOnline()) {
        error_->setText(tr("Not connected to the server. Check the address in Settings."));
        return;
    }
    if (server_->isLoggedIn()) server_->logout();
    pendingId_ = registering_ ? server_->registerAccount(n, p) : server_->login(n, p);
    submit_->setEnabled(false);
    error_->setText(tr("Contacting the server..."));
}

void LoginDialog::onMessage(const QJsonObject& msg) {
    if (pendingId_ < 0 || msg.value(QStringLiteral("id")).toInt(-2) != pendingId_) return;
    pendingId_ = -1;
    submit_->setEnabled(true);
    if (msg.value(QStringLiteral("type")).toString() == QLatin1String("account")) {
        password_->clear();
        confirm_->clear();
        accept();
    } else {
        error_->setText(msg.value(QStringLiteral("message")).toString());
    }
}

// --- MatchFoundDialog ------------------------------------------------------------------------------

MatchFoundDialog::MatchFoundDialog(const QJsonObject& found, const QString& gameTitle, const QString& modeName,
                                   const QString& characterName, QWidget* parent)
    : QDialog(parent) {
    matchId_ = found.value(QStringLiteral("match")).toString();
    setWindowTitle(tr("Match found"));
    setModal(false);
    setMinimumWidth(460);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(28, 22, 28, 22);
    v->setSpacing(10);
    auto* emblem = new QLabel;
    emblem->setPixmap(style::logo(64));
    emblem->setAlignment(Qt::AlignCenter);
    v->addWidget(emblem);
    auto* big = ui::label(tr("MATCH FOUND"), "MatchFound");
    big->setAlignment(Qt::AlignCenter);
    ui::letterSpacing(big, 6);
    v->addWidget(big);
    const bool ranked = found.value(QStringLiteral("ranked")).toBool();
    auto* what = ui::label(QStringLiteral("%1  |  %2  |  %3").arg(gameTitle, modeName, ranked ? tr("Ranked") : tr("Kasual")),
                           "Muted");
    what->setAlignment(Qt::AlignCenter);
    v->addWidget(what);
    const QJsonObject opp = found.value(QStringLiteral("opponent")).toObject();
    QString vs;
    if (opp.isEmpty())
        vs = tr("Your opponent is revealed when both players accept.");
    else
        vs = tr("vs  %1").arg(opp.value(QStringLiteral("name")).toString());
    auto* vsLabel = ui::label(vs, "Big");
    vsLabel->setAlignment(Qt::AlignCenter);
    vsLabel->setWordWrap(true);
    v->addWidget(vsLabel);
    if (found.contains(QStringLiteral("connection"))) {
        // the opponent's connection: the link's icon (Wi-Fi fan or wired bars), then the ping in words
        const QString link = found.value(QStringLiteral("connection")).toObject().value(QStringLiteral("link")).toString();
        auto* row = new QHBoxLayout;
        row->setSpacing(8);
        row->addStretch();
        const QPixmap icon = style::linkIcon(link, 22);
        if (!icon.isNull()) {
            auto* iconLabel = new QLabel;
            iconLabel->setPixmap(icon);
            iconLabel->setToolTip(style::linkName(link));
            iconLabel->setAccessibleName(style::linkName(link));
            row->addWidget(iconLabel);
        }
        QString text = MatchController::pingText(found);
        if (icon.isNull()) text = text.isEmpty() ? style::linkName(link) : style::linkName(link) + QStringLiteral("  ·  ") + text;
        if (!text.isEmpty()) row->addWidget(ui::label(text, link == QLatin1String("wifi") ? "Warning" : "Muted"));
        row->addStretch();
        v->addLayout(row);
    }
    if (!characterName.isEmpty()) {
        auto* you = ui::label(tr("You play %1").arg(characterName), "Muted");
        you->setAlignment(Qt::AlignCenter);
        v->addWidget(you);
    }
    bar_ = new QProgressBar;
    bar_->setTextVisible(false);
    bar_->setRange(0, 1000);
    v->addWidget(bar_);
    countdown_ = ui::label(QString(), "Muted");
    countdown_->setAlignment(Qt::AlignCenter);
    v->addWidget(countdown_);
    auto* buttons = new QHBoxLayout;
    auto* decline = new QPushButton(tr("Decline"));
    auto* acceptBtn = new QPushButton(tr("ACCEPT"));
    acceptBtn->setObjectName(QStringLiteral("Primary"));
    acceptBtn->setDefault(true);
    buttons->addWidget(decline);
    buttons->addStretch();
    buttons->addWidget(acceptBtn);
    v->addLayout(buttons);
    connect(acceptBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(decline, &QPushButton::clicked, this, &QDialog::reject);
    acceptBtn->setFocus();  // A on a controller, as Enter on the keyboard, accepts

    const int timeout = qMax(1, found.value(QStringLiteral("accept_timeout")).toInt(15));
    deadlineMs_ = QDateTime::currentMSecsSinceEpoch() + timeout * 1000;
    tick_.setInterval(100);
    connect(&tick_, &QTimer::timeout, this, [this, timeout] {
        const qint64 left = deadlineMs_ - QDateTime::currentMSecsSinceEpoch();
        bar_->setValue(static_cast<int>(std::clamp<qint64>(left * 1000 / (qint64(timeout) * 1000), 0, 1000)));
        countdown_->setText(tr("%1 s to accept").arg(qMax<qint64>(0, (left + 999) / 1000)));
        if (left <= 0) {
            tick_.stop();
            done(2);  // neither accept nor decline: the server cancels the match
        }
    });
    tick_.start();
    winutil::useDarkTitleBar(this);
}

// --- RematchDialog ---------------------------------------------------------------------------------

RematchDialog::RematchDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Rematch?"));
    setModal(false);
    setMinimumWidth(480);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(28, 22, 28, 22);
    v->setSpacing(10);
    auto* emblem = new QLabel;
    emblem->setPixmap(style::logo(64));
    emblem->setAlignment(Qt::AlignCenter);
    v->addWidget(emblem);
    title_ = ui::label(QString(), "CardTitle");
    title_->setAlignment(Qt::AlignCenter);
    v->addWidget(title_);
    score_ = ui::label(QString(), "Big");
    score_->setAlignment(Qt::AlignCenter);
    v->addWidget(score_);
    info_ = ui::label(QString(), "Muted");
    info_->setAlignment(Qt::AlignCenter);
    info_->setWordWrap(true);
    v->addWidget(info_);
    auto* buttons = new QHBoxLayout;
    rematch_ = new QPushButton(tr("REMATCH"));
    rematch_->setObjectName(QStringLiteral("Primary"));
    rematch_->setProperty("compact", true);
    rematch_->setMinimumHeight(40);
    rematch_->setDefault(true);
    rematch_->setToolTip(tr("Fight again with the same fighters (when you both choose it)"));
    select_ = new QPushButton(tr("SELECT"));
    select_->setMinimumHeight(40);
    select_->setToolTip(tr("Both go back to character select to pick again"));
    auto* leave = new QPushButton(tr("LEAVE"));
    leave->setMinimumHeight(40);
    buttons->addWidget(rematch_, 1);
    buttons->addWidget(select_, 1);
    buttons->addWidget(leave, 1);
    v->addLayout(buttons);
    connect(rematch_, &QPushButton::clicked, this, [this] { emit chose(QStringLiteral("rematch")); });
    connect(select_, &QPushButton::clicked, this, [this] { emit chose(QStringLiteral("select")); });
    connect(leave, &QPushButton::clicked, this, [this] { emit chose(QStringLiteral("leave")); });
    rematch_->setFocus();  // A on a controller, as Enter on the keyboard, chooses the rematch
    winutil::useDarkTitleBar(this);
}

void RematchDialog::update(const QString& title, const QString& score, const QString& info, bool canChoose,
                           bool canSelect) {
    title_->setText(title);
    score_->setText(score);
    info_->setText(info);
    rematch_->setEnabled(canChoose);
    select_->setEnabled(canChoose);
    select_->setVisible(canSelect);
}

// --- UpdateDialog ----------------------------------------------------------------------------------

UpdateDialog::UpdateDialog(const QJsonObject& release, bool required, const QString& blocker, QWidget* parent)
    : QDialog(parent) {
    const QString version = release.value(QStringLiteral("version")).toString();
    const QString page = release.value(QStringLiteral("page")).toString();
    setWindowTitle(tr("MK3D Online %1").arg(version));
    setModal(false);
    setMinimumWidth(500);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(28, 22, 28, 22);
    v->setSpacing(10);
    auto* emblem = new QLabel;
    emblem->setPixmap(style::logo(64));
    emblem->setAlignment(Qt::AlignCenter);
    v->addWidget(emblem);
    auto* title = ui::label(required ? tr("UPDATE NEEDED") : tr("UPDATE AVAILABLE"), "CardTitle");
    title->setAlignment(Qt::AlignCenter);
    v->addWidget(title);
    auto* big = ui::label(tr("MK3D Online %1").arg(version), "Big");
    big->setAlignment(Qt::AlignCenter);
    v->addWidget(big);
    auto* info = ui::label(required ? tr("You have %1. The server needs this version to play online.")
                                          .arg(QStringLiteral(MK3D_LAUNCHER_VERSION))
                                    : tr("You have %1.").arg(QStringLiteral(MK3D_LAUNCHER_VERSION)),
                           "Muted");
    info->setAlignment(Qt::AlignCenter);
    info->setWordWrap(true);
    v->addWidget(info);
    const QString notes = release.value(QStringLiteral("notes")).toString().trimmed();
    if (!notes.isEmpty()) {
        auto* text = new QLabel(notes.left(700));
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        v->addWidget(text);
    }
    if (page.startsWith(QLatin1String("https://"))) {
        auto* link = new QLabel(QStringLiteral("<a href=\"%1\" style=\"color:#F2C14E\">%2</a>")
                                    .arg(page.toHtmlEscaped(), tr("What's new in %1").arg(version)));
        link->setTextFormat(Qt::RichText);
        link->setOpenExternalLinks(true);
        link->setAlignment(Qt::AlignCenter);
        v->addWidget(link);
    }
    status_ = ui::label(blocker.isEmpty() ? tr("The launcher downloads it, checks it, puts it in place and starts "
                                               "again. Your settings, saves and built games stay.")
                                          : blocker,
                        "Muted");
    status_->setWordWrap(true);
    v->addWidget(status_);
    bar_ = new QProgressBar;
    bar_->setTextVisible(false);
    bar_->setFixedHeight(8);
    bar_->hide();
    v->addWidget(bar_);
    auto* buttons = new QHBoxLayout;
    now_ = new QPushButton(blocker.isEmpty() ? tr("UPDATE NOW") : tr("OPEN THE DOWNLOAD PAGE"));
    now_->setObjectName(QStringLiteral("Primary"));
    now_->setProperty("compact", true);
    now_->setMinimumHeight(40);
    now_->setDefault(true);
    later_ = new QPushButton(tr("LATER"));
    later_->setMinimumHeight(40);
    buttons->addWidget(now_, 1);
    buttons->addWidget(later_, 1);
    v->addLayout(buttons);
    connect(now_, &QPushButton::clicked, this, [this, blocker, page] {
        if (!blocker.isEmpty()) {
            if (!page.isEmpty()) QDesktopServices::openUrl(QUrl(page));
            return;
        }
        running_ = true;
        now_->setEnabled(false);
        later_->setText(tr("CANCEL"));
        bar_->show();
        emit updateNow();
    });
    connect(later_, &QPushButton::clicked, this, [this] {
        if (running_) {
            emit cancelRequested();
            running_ = false;
            now_->setEnabled(true);
            now_->setText(tr("UPDATE NOW"));
            later_->setText(tr("LATER"));
            bar_->hide();
            status_->setText(tr("Cancelled: nothing was changed."));
            return;
        }
        reject();
    });
    now_->setFocus();  // A on a controller, as Enter on the keyboard, updates
    winutil::useDarkTitleBar(this);
}

void UpdateDialog::setProgress(const QString& text, int percent) {
    status_->setText(text);
    bar_->show();
    bar_->setRange(0, percent < 0 ? 0 : 100);  // 0..0: busy without a measure
    if (percent >= 0) bar_->setValue(percent);
}

void UpdateDialog::setWaiting(const QString& text) {
    status_->setText(text);
    bar_->setRange(0, 100);
    bar_->setValue(100);
}

void UpdateDialog::setFailed(const QString& text) {
    running_ = false;
    status_->setText(text);
    bar_->hide();
    now_->setEnabled(true);
    now_->setText(tr("TRY AGAIN"));
    later_->setText(tr("LATER"));
}

// --- CreateLobbyDialog -----------------------------------------------------------------------------

CreateLobbyDialog::CreateLobbyDialog(const Game& game, const QString& defaultName, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Create lobby"));
    setMinimumWidth(400);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 18, 22, 18);
    auto* t = ui::label(tr("NEW LOBBY"), "PageTitle");
    ui::letterSpacing(t, 3);
    v->addWidget(t);
    v->addWidget(ui::label(game.title, "Muted"));
    auto* form = new QFormLayout;
    name_ = new QLineEdit(defaultName);
    name_->setMaxLength(32);
    mode_ = new QComboBox;
    for (const auto& m : game.onlineModes()) mode_->addItem(m.name, m.id);
    password_ = new QLineEdit;
    password_->setPlaceholderText(tr("optional"));
    password_->setMaxLength(64);
    rounds_ = new QComboBox;
    for (int r : {1, 3, 5}) rounds_->addItem(tr("Best of %1").arg(r), r);
    rounds_->setCurrentIndex(1);
    deathTraps_ = new QCheckBox(tr("Death traps"));
    deathTraps_->setChecked(cfg::deathTraps());
    deathTraps_->setToolTip(tr("Arenas whose death traps kill on the spot, in this lobby's matches."));
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Mode"), mode_);
    form->addRow(tr("Password"), password_);
    form->addRow(tr("Rounds"), rounds_);
    form->addRow(QString(), deathTraps_);
    v->addLayout(form);
    // only for a mode whose fights have them
    const auto showTraps = [this, modes = game.modes] {
        const QString id = mode_->currentData().toString();
        bool traps = false;
        for (const auto& m : modes)
            if (m.id == id) traps = m.deathTraps;
        deathTraps_->setVisible(traps);
    };
    showTraps();
    connect(mode_, &QComboBox::currentIndexChanged, this, showTraps);
    auto* buttons = new QHBoxLayout;
    auto* cancel = new QPushButton(tr("Cancel"));
    auto* ok = new QPushButton(tr("Create"));
    ok->setDefault(true);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(ok);
    v->addLayout(buttons);
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    winutil::useDarkTitleBar(this);
}

QString CreateLobbyDialog::name() const { return name_->text().trimmed(); }
QString CreateLobbyDialog::mode() const { return mode_->currentData().toString(); }
QString CreateLobbyDialog::password() const { return password_->text(); }
int CreateLobbyDialog::rounds() const { return rounds_->currentData().toInt(); }
bool CreateLobbyDialog::deathTraps() const { return deathTraps_->isHidden() || deathTraps_->isChecked(); }
