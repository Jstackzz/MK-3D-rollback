#include "ToolPages.h"

#include <QCheckBox>
#include <QCryptographicHash>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QStorageInfo>
#include <QThread>
#include <QVBoxLayout>

#include "AppPaths.h"
#include "GameData.h"
#include "GameProcess.h"
#include "ServerClient.h"
#include "Upnp.h"

namespace {
QString native(const QString& p) { return QDir::toNativeSeparators(p); }

QString humanBytes(qint64 b) {
    if (b >= (qint64(1) << 30)) return QStringLiteral("%1 GB").arg(double(b) / double(qint64(1) << 30), 0, 'f', 2);
    if (b >= (1 << 20)) return QStringLiteral("%1 MB").arg(double(b) / double(1 << 20), 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(b / 1024);
}

QWidget* pathRow(QLineEdit* edit, QPushButton* browse) {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(edit, 1);
    h->addWidget(browse);
    return w;
}

// the resolution box's text as the games read MK3D_RESOLUTION: "desktop" or WIDTHxHEIGHT; empty while it is neither
QString gameResolution(const QString& text) {
    const QString t = text.trimmed();
    if (t.isEmpty() || t.compare(QLatin1String("desktop"), Qt::CaseInsensitive) == 0) return QStringLiteral("desktop");
    static const QRegularExpression size(QStringLiteral("^(\\d{3,5})\\s*[xX\\x{00D7}*]\\s*(\\d{3,5})$"));
    const QRegularExpressionMatch m = size.match(t);
    const int w = m.captured(1).toInt(), h = m.captured(2).toInt();
    if (!m.hasMatch() || w < 320 || h < 240 || w > 16384 || h > 16384) return {};
    return QStringLiteral("%1x%2").arg(w).arg(h);
}
}  // namespace

// --- BuildPage -------------------------------------------------------------------------------------

BuildPage::BuildPage(QWidget* parent) : Page(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 20, 28, 16);
    root->setSpacing(14);
    root->addWidget(ui::pageHeader(tr("Build game"),
                                   tr("The game is built on your PC from your own disc: the disc image is "
                                      "extracted into the game's assets folder, then the first launch compiles "
                                      "the game's code library. Nothing from the game is downloaded.")));
    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    root->addLayout(columns);

    QVBoxLayout* info = nullptr;
    columns->addWidget(ui::card(tr("Game"), &info), 1);
    gameTitle_ = ui::label(QString(), "Big");
    folderInfo_ = ui::label(QString(), "Muted");
    assetsInfo_ = ui::label(QString(), "Muted");
    exeInfo_ = ui::label(QString(), "Muted");
    for (QLabel* l : {folderInfo_, assetsInfo_, exeInfo_}) {
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    info->addWidget(gameTitle_);
    info->addWidget(folderInfo_);
    info->addWidget(assetsInfo_);
    info->addWidget(exeInfo_);
    info->addStretch();

    QVBoxLayout* steps = nullptr;
    columns->addWidget(ui::card(tr("Steps"), &steps), 1);
    steps->addWidget(ui::label(tr("1. Your disc image (.iso or .xiso.iso)"), "Muted"));
    image_ = new QLineEdit;
    image_->setPlaceholderText(tr("path to your disc image"));
    browse_ = new QPushButton(tr("Browse..."));
    steps->addWidget(pathRow(image_, browse_));
    extract_ = new QPushButton(tr("EXTRACT AND BUILD"));
    extract_->setObjectName(QStringLiteral("Primary"));
    steps->addWidget(extract_);
    auto* compileNote = ui::label(tr("2. The game then starts once and prepares its code for this PC (a few "
                                     "minutes). When it says the game is ready, choose Exit. Again, by hand:"), "Muted");
    compileNote->setWordWrap(true);
    steps->addWidget(compileNote);
    compile_ = new QPushButton(tr("Launch once to compile"));
    steps->addWidget(compile_);
    cancel_ = new QPushButton(tr("Cancel"));
    cancel_->setEnabled(false);
    steps->addWidget(cancel_, 0, Qt::AlignRight);
    steps->addStretch();

    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    progress_->setFormat(QStringLiteral("%p%"));
    root->addWidget(progress_);
    log_ = new LogView;
    root->addWidget(log_, 1);

    connect(browse_, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Disc image"), image_->text(),
                                                       tr("Xbox disc images (*.iso *.xiso);;All files (*)"));
        if (!f.isEmpty()) {
            image_->setText(native(f));
            cfg::set(QStringLiteral("games/%1/image").arg(gameId_), f);
        }
    });
    connect(extract_, &QPushButton::clicked, this, &BuildPage::startExtract);
    connect(compile_, &QPushButton::clicked, this, &BuildPage::compileFirstLaunch);
    connect(cancel_, &QPushButton::clicked, this, [this] {
        stop_ = true;
        if (firstRun_) firstRun_->kill();
    });
    progressTimer_.setInterval(500);
    connect(&progressTimer_, &QTimer::timeout, this, &BuildPage::updateProgress);
}

BuildPage::~BuildPage() {
    stop_ = true;
    if (worker_) worker_->wait();
}

void BuildPage::extractImage(const QString& path) {
    image_->setText(native(path));
    startExtract();
}

void BuildPage::setGame(const QString& gameId) {
    if (step_ != Step::Idle) return;  // keep the running build's game
    Page::setGame(gameId);
    image_->setText(native(cfg::get(QStringLiteral("games/%1/image").arg(gameId)).toString()));
    refreshInfo();
}

void BuildPage::refreshInfo() {
    const Game* g = GameData::instance().game(gameId_);
    if (!g) return;
    gameTitle_->setText(g->title);
    const QString folder = cfg::gameFolder(gameId_);
    const QString assets = QDir(folder).filePath(QStringLiteral("assets"));
    folderInfo_->setText(tr("%1 (%2), title ID %3\nGame folder: %4").arg(g->title, g->edition, g->titleId, native(folder)));
    if (QFileInfo::exists(QDir(assets).filePath(QStringLiteral("default.xbe"))))
        assetsInfo_->setText(tr("Assets: %1 (default.xbe present)").arg(native(assets)));
    else
        assetsInfo_->setText(tr("Assets: %1 (not extracted yet)").arg(native(assets)));
    const QFileInfo exe(cfg::gameExe(gameId_));
    const bool built = QFileInfo::exists(exe.dir().filePath(exe.completeBaseName() + QStringLiteral(".dll")));
    if (!exe.exists())
        exeInfo_->setText(tr("Executable: %1 (missing)").arg(native(exe.filePath())));
    else
        exeInfo_->setText(tr("Executable: %1\nGame code: %2").arg(native(exe.filePath()),
                                                                built ? tr("built") : tr("not built yet (the first start builds it)")));
}

void BuildPage::setBusy(bool busy) {
    extract_->setEnabled(!busy);
    compile_->setEnabled(!busy);
    browse_->setEnabled(!busy);
    image_->setEnabled(!busy);
    cancel_->setEnabled(busy);
    emit busyChanged(busy);
}

void BuildPage::runWorker(std::function<void()> work, std::function<void()> done) {
    worker_ = QThread::create(std::move(work));
    worker_->setParent(this);
    connect(worker_, &QThread::finished, this, [this, done] {
        worker_->deleteLater();
        worker_ = nullptr;
        done();
    });
    worker_->start();
}

void BuildPage::finishStep() {
    step_ = Step::Idle;
    setBusy(false);
}

void BuildPage::startExtract() {
    if (step_ != Step::Idle) return;
    imagePath_ = QDir::fromNativeSeparators(image_->text().trimmed());
    if (!QFileInfo(imagePath_).isFile()) {
        log_->line(tr("Choose your disc image first."));
        return;
    }
    cfg::set(QStringLiteral("games/%1/image").arg(gameId_), imagePath_);
    outDir_ = QDir(cfg::gameFolder(gameId_)).filePath(QStringLiteral("assets"));
    progress_->setValue(0);
    progress_->setFormat(QStringLiteral("%p%"));
    log_->line(tr("Reading the disc's file table: %1").arg(native(imagePath_)));
    applog::write(QStringLiteral("build %1: reading %2").arg(gameId_, native(imagePath_)));
    step_ = Step::Reading;
    stop_ = false;
    setBusy(true);
    auto disc = std::make_shared<xiso::Image>();
    auto result = std::make_shared<WorkResult>();
    runWorker(
        [disc, result, path = imagePath_] {
            if (!disc->open(path, &result->error)) return;
            const xiso::Entry* xbe = disc->find(QStringLiteral("default.xbe"));
            if (!xbe) {
                result->error = tr("The disc has no default.xbe: this is not a game disc.");
                return;
            }
            QByteArray data;
            if (!disc->read(*xbe, &data, &result->error)) return;
            result->sha1 = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
            result->ok = true;
        },
        [this, disc, result] { onDiscRead(disc, *result); });
}

void BuildPage::onDiscRead(const std::shared_ptr<xiso::Image>& disc, const WorkResult& r) {
    if (!r.ok) {
        log_->line(r.error);
        applog::write(QStringLiteral("build %1: %2").arg(gameId_, r.error));
        finishStep();
        return;
    }
    totalBytes_ = disc->totalBytes();
    totalFiles_ = disc->fileCount();
    log_->line(tr("%1 files, %2; default.xbe SHA-1 %3.").arg(totalFiles_).arg(humanBytes(totalBytes_), r.sha1));
    const Game* g = GameData::instance().game(gameId_);
    if (g && !g->xbeSha1.isEmpty() && r.sha1.compare(g->xbeSha1, Qt::CaseInsensitive) != 0) {
        log_->line(tr("This is not the release this build is made for (%1, %2: default.xbe SHA-1 %3).")
                       .arg(g->title, g->edition, g->xbeSha1));
        applog::write(QStringLiteral("build %1: another release, default.xbe SHA-1 %2").arg(gameId_, r.sha1));
        const auto answer = QMessageBox::warning(
            this, tr("Different release"),
            tr("This disc is not %1 (%2), the release this build of the game is made for. The game will most "
               "likely crash with it.\n\nExtract it anyway?").arg(g->title, g->edition),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            finishStep();
            return;
        }
    }
    QDir().mkpath(outDir_);
    const qint64 freeBytes = QStorageInfo(outDir_).bytesAvailable();
    const qint64 needed = totalBytes_ + (qint64(1) << 30);  // the files, and the game code the first start builds
    if (freeBytes >= 0 && freeBytes < needed) {
        log_->line(tr("Not enough free space on that drive: %1 needed, %2 free.").arg(humanBytes(needed), humanBytes(freeBytes)));
        finishStep();
        return;
    }
    log_->line(tr("Extracting into %1 ...").arg(native(outDir_)));
    step_ = Step::Extracting;
    bytesDone_ = 0;
    filesDone_ = 0;
    progressTimer_.start();
    auto result = std::make_shared<WorkResult>();
    runWorker([this, disc, result, out = outDir_] {
        result->ok = disc->extractAll(out, stop_, bytesDone_, filesDone_, &result->error);
    }, [this, result] { onExtracted(*result); });
}

void BuildPage::onExtracted(const WorkResult& r) {
    progressTimer_.stop();
    finishStep();
    if (!r.ok) {
        log_->line(tr("Extraction stopped: %1").arg(r.error));
        applog::write(QStringLiteral("build %1: extraction stopped: %2").arg(gameId_, r.error));
        refreshInfo();
        return;
    }
    progress_->setValue(1000);
    progress_->setFormat(tr("Done  -  %1 files, %2").arg(totalFiles_).arg(humanBytes(totalBytes_)));
    log_->line(tr("Extraction finished."));
    applog::write(QStringLiteral("build %1: extracted %2 files into %3").arg(gameId_).arg(totalFiles_).arg(native(outDir_)));
    refreshInfo();
    compileFirstLaunch();
}

void BuildPage::updateProgress() {
    if (step_ != Step::Extracting || totalBytes_ <= 0) return;
    const qint64 done = bytesDone_;
    progress_->setValue(static_cast<int>(qMin<qint64>(999, done * 1000 / totalBytes_)));
    progress_->setFormat(tr("%p%  -  %1 of %2 files, %3 of %4").arg(filesDone_.load()).arg(totalFiles_)
                             .arg(humanBytes(done), humanBytes(totalBytes_)));
}

void BuildPage::compileFirstLaunch() {
    if (firstRun_ || step_ != Step::Idle) return;
    const QString exe = cfg::gameExe(gameId_);
    if (!QFileInfo::exists(QDir(cfg::gameFolder(gameId_)).filePath(QStringLiteral("assets/default.xbe"))))
        log_->line(tr("Warning: assets/default.xbe is missing; extract the disc first."));
    firstRun_ = new GameProcess(gameId_, 1, this);
    connect(firstRun_, &GameProcess::outputLine, log_, &LogView::line);
    connect(firstRun_, &GameProcess::exited, this, [this](int code, bool crashed) {
        log_->line(crashed ? tr("The game crashed (code %1).").arg(code) : tr("The game exited (code %1).").arg(code));
        applog::write(QStringLiteral("build %1: first launch exited with code %2%3").arg(gameId_).arg(code).arg(crashed ? " (crash)" : ""));
        firstRun_->deleteLater();
        firstRun_ = nullptr;
        finishStep();
        refreshInfo();
    });
    QString err;
    if (!firstRun_->start(exe, &err, false)) {
        log_->line(err);
        delete firstRun_;
        firstRun_ = nullptr;
        return;
    }
    step_ = Step::Compiling;
    setBusy(true);
    log_->line(tr("Started %1. The first start prepares the game's code for this PC, a few minutes; when it says "
                  "the game is ready, choose Exit (or Start to try it).").arg(native(exe)));
}

// --- SettingsPage ----------------------------------------------------------------------------------

SettingsPage::SettingsPage(ServerClient* server, QWidget* parent) : Page(parent), server_(server) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(28, 20, 28, 20);
    root->setSpacing(10);
    root->addWidget(ui::pageHeader(tr("Settings"), tr("Saved to %1").arg(native(paths::configFile()))));
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(4);
    root->addLayout(grid);

    // server
    auto* serverBox = new QGroupBox(tr("SERVER"));
    auto* sf = new QFormLayout(serverBox);
    address_ = new QLineEdit;
    address_->setPlaceholderText(QStringLiteral("host:7777"));
    fingerprint_ = new QLineEdit;
    fingerprint_->setPlaceholderText(tr("SHA-256 of the server certificate (the server's host gives it out)"));
    autoConnect_ = new QCheckBox(tr("Connect when the launcher starts"));
    autoConnect_->setChecked(cfg::get(QStringLiteral("server/auto_connect"), true).toBool());
    auto* connectBtn = new QPushButton(tr("Connect"));
    auto* disconnectBtn = new QPushButton(tr("Disconnect"));
    auto* pinBtn = new QPushButton(tr("Pin presented certificate"));
    auto* btnRow = new QWidget;
    auto* br = new QHBoxLayout(btnRow);
    br->setContentsMargins(0, 0, 0, 0);
    br->addWidget(connectBtn);
    br->addWidget(disconnectBtn);
    br->addWidget(pinBtn);
    br->addStretch();
    serverState_ = ui::label(QString(), "Muted");
    presented_ = ui::label(QString(), "Muted");
    presented_->setWordWrap(true);
    presented_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sf->addRow(tr("Address"), address_);
    sf->addRow(tr("Fingerprint"), fingerprint_);
    sf->addRow(QString(), autoConnect_);
    sf->addRow(QString(), btnRow);
    sf->addRow(tr("State"), serverState_);
    sf->addRow(tr("Presented"), presented_);
    grid->addWidget(serverBox, 0, 0, 1, 2);

    // online
    auto* onlineBox = new QGroupBox(tr("ONLINE"));
    auto* of = new QFormLayout(onlineBox);
    auto* region = new QComboBox;
    ui::fillRegions(region, cfg::region());
    auto* maxPing = new QComboBox;
    ui::fillMaxPing(maxPing, cfg::maxPing());
    auto* delay = new QSpinBox;
    delay->setRange(0, 4);
    delay->setValue(cfg::inputDelay());
    delay->setSuffix(tr(" frames"));
    auto* port = new QSpinBox;
    port->setRange(1024, 65535);
    port->setValue(cfg::gamePort());
    auto* strict = new QCheckBox(tr("Strict ranked matchmaking by default"));
    strict->setChecked(cfg::strict());
    auto* upnp = new QCheckBox(tr("Open the game port on the router (UPnP)"));
    upnp->setChecked(cfg::upnp());
    routerState_ = ui::label(tr("Router: not asked"), "Muted");
    routerState_->setWordWrap(true);
    of->addRow(tr("Region"), region);
    of->addRow(tr("Max ping"), maxPing);
    of->addRow(tr("Input delay"), delay);
    of->addRow(tr("Game UDP port"), port);
    of->addRow(QString(), strict);
    of->addRow(QString(), upnp);
    of->addRow(tr("Router"), routerState_);
    auto* delayNote = ui::label(tr("Ranked uses at least 1 frame. Players who cannot reach each other directly "
                                   "play through the server, with a little more delay."), "Muted");
    delayNote->setWordWrap(true);
    of->addRow(QString(), delayNote);
    grid->addWidget(onlineBox, 1, 0);

    // display
    auto* displayBox = new QGroupBox(tr("DISPLAY"));
    auto* df = new QFormLayout(displayBox);
    auto* mode = new QComboBox;
    mode->addItem(tr("Windowed"), QStringLiteral("windowed"));
    mode->addItem(tr("Borderless"), QStringLiteral("borderless"));
    mode->addItem(tr("Fullscreen"), QStringLiteral("fullscreen"));
    mode->setCurrentIndex(qMax(0, mode->findData(cfg::displayMode())));
    auto* res = new QComboBox;
    res->setEditable(true);
    res->addItem(tr("Desktop"), QStringLiteral("desktop"));
    for (const char* size : {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"})
        res->addItem(QLatin1String(size), QLatin1String(size));
    if (const int at = res->findData(cfg::resolution()); at >= 0) res->setCurrentIndex(at);
    else res->setCurrentText(cfg::resolution());
    auto* vsync = new QCheckBox(tr("V-sync"));
    vsync->setChecked(cfg::vsync());
    df->addRow(tr("Window"), mode);
    df->addRow(tr("Resolution"), res);
    df->addRow(QString(), vsync);
    auto* displayNote = ui::label(tr("A game takes these when it starts. Desktop is your monitor's own resolution (a "
                                     "window opens at 1280x720 then); type any other size as 1600x1200."), "Muted");
    displayNote->setWordWrap(true);
    df->addRow(QString(), displayNote);
    grid->addWidget(displayBox, 1, 1);

    // games
    auto* gamesBox = new QGroupBox(tr("GAME FOLDERS"));
    auto* gf = new QFormLayout(gamesBox);
    for (const Game& g : GameData::instance().games()) {
        const QString id = g.id;
        auto* folder = new QLineEdit(native(cfg::gameFolder(id)));
        auto* folderBrowse = new QPushButton(tr("..."));
        auto* exe = new QLineEdit(native(cfg::gameExe(id)));
        auto* exeBrowse = new QPushButton(tr("..."));
        gf->addRow(ui::label(g.title, "CardTitle"));
        gf->addRow(tr("Folder"), pathRow(folder, folderBrowse));
        gf->addRow(tr("Executable"), pathRow(exe, exeBrowse));
        connect(folder, &QLineEdit::editingFinished, this, [id, folder] {
            cfg::set(QStringLiteral("games/%1/folder").arg(id), QDir::fromNativeSeparators(folder->text().trimmed()));
        });
        connect(exe, &QLineEdit::editingFinished, this, [id, exe] {
            cfg::set(QStringLiteral("games/%1/exe").arg(id), QDir::fromNativeSeparators(exe->text().trimmed()));
        });
        connect(folderBrowse, &QPushButton::clicked, this, [this, id, folder] {
            const QString d = QFileDialog::getExistingDirectory(this, tr("Game folder"), folder->text());
            if (d.isEmpty()) return;
            folder->setText(native(d));
            cfg::set(QStringLiteral("games/%1/folder").arg(id), d);
        });
        connect(exeBrowse, &QPushButton::clicked, this, [this, id, exe] {
            const QString f = QFileDialog::getOpenFileName(this, tr("Game executable"), exe->text(),
                                                           tr("Programs (*.exe *.py);;All files (*)"));
            if (f.isEmpty()) return;
            exe->setText(native(f));
            cfg::set(QStringLiteral("games/%1/exe").arg(id), f);
        });
    }
    grid->addWidget(gamesBox, 2, 0, 1, 2);

    // tools and development
    auto* toolsBox = new QGroupBox(tr("TOOLS AND DEVELOPMENT"));
    auto* tf = new QFormLayout(toolsBox);
    auto* python = new QLineEdit(cfg::python());
    auto* repo = new QLineEdit(native(cfg::repoRoot()));
    auto* lp1 = new QSpinBox;
    lp1->setRange(1024, 65535);
    lp1->setValue(cfg::localTestPort(1));
    auto* lp2 = new QSpinBox;
    lp2->setRange(1024, 65535);
    lp2->setValue(cfg::localTestPort(2));
    tf->addRow(tr("Python"), python);
    tf->addRow(tr("Repository"), repo);
    tf->addRow(tr("Local test ports"), [&] {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(lp1);
        h->addWidget(lp2);
        h->addStretch();
        return w;
    }());
    grid->addWidget(toolsBox, 3, 0, 1, 2);
    toolsBox->setVisible(!paths::isPackage());  // a player package has no repository or Python to point at
    root->addStretch();

    connect(region, &QComboBox::currentIndexChanged, this, [region] { cfg::set(QStringLiteral("online/region"), region->currentData()); });
    connect(maxPing, &QComboBox::currentIndexChanged, this, [maxPing] { cfg::set(QStringLiteral("online/max_ping"), maxPing->currentData()); });
    connect(delay, &QSpinBox::valueChanged, this, [](int v) { cfg::set(QStringLiteral("online/delay"), v); });
    connect(port, &QSpinBox::valueChanged, this, [this](int v) {
        cfg::set(QStringLiteral("online/game_port"), v);
        if (upnp_ && cfg::upnp()) upnp_->map(static_cast<quint16>(v));
    });
    connect(upnp, &QCheckBox::toggled, this, [this](bool on) {
        cfg::set(QStringLiteral("online/upnp"), on);
        if (on && upnp_) upnp_->map(static_cast<quint16>(cfg::gamePort()));
    });
    connect(strict, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("online/strict"), on); });
    connect(mode, &QComboBox::currentIndexChanged, this, [mode] { cfg::set(QStringLiteral("display/mode"), mode->currentData()); });
    connect(res, &QComboBox::currentTextChanged, this, [](const QString& t) {
        if (const QString size = gameResolution(t); !size.isEmpty()) cfg::set(QStringLiteral("display/resolution"), size);
    });
    connect(vsync, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("display/vsync"), on); });
    connect(python, &QLineEdit::editingFinished, this, [python] { cfg::set(QStringLiteral("tools/python"), python->text().trimmed()); });
    connect(repo, &QLineEdit::editingFinished, this, [repo] {
        cfg::set(QStringLiteral("tools/repo_root"), QDir::fromNativeSeparators(repo->text().trimmed()));
    });
    connect(lp1, &QSpinBox::valueChanged, this, [](int v) { cfg::set(QStringLiteral("dev/local_port1"), v); });
    connect(lp2, &QSpinBox::valueChanged, this, [](int v) { cfg::set(QStringLiteral("dev/local_port2"), v); });
    connect(autoConnect_, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("server/auto_connect"), on); });
    connect(connectBtn, &QPushButton::clicked, this, [this] {
        cfg::set(QStringLiteral("server/address"), address_->text().trimmed());
        cfg::set(QStringLiteral("server/fingerprint"), ServerClient::normalizeFingerprint(fingerprint_->text()));
        emit reconnectRequested();
    });
    connect(disconnectBtn, &QPushButton::clicked, server_, &ServerClient::disconnectFromServer);
    connect(pinBtn, &QPushButton::clicked, this, [this] {
        if (server_->peerFingerprint().isEmpty()) return;
        fingerprint_->setText(ServerClient::prettyFingerprint(server_->peerFingerprint()));
    });
    connect(server_, &ServerClient::stateChanged, this, [this] { reloadServer(); });
    reloadServer();
}

void SettingsPage::setGame(const QString& gameId) { Page::setGame(gameId); }

void SettingsPage::setUpnp(UpnpMapper* upnp) {
    upnp_ = upnp;
    const auto show = [this](const QString& text) {
        routerState_->setText(text.isEmpty() ? tr("not asked (UPnP is off)") : text);
    };
    connect(upnp_, &UpnpMapper::statusChanged, this, show);
    show(upnp_->status());
}

void SettingsPage::reloadServer() {
    if (!address_->hasFocus()) address_->setText(cfg::serverAddress());
    if (!fingerprint_->hasFocus()) fingerprint_->setText(ServerClient::prettyFingerprint(cfg::fingerprint()));
    static const char* names[] = {"offline", "connecting", "connected", "signed in"};
    serverState_->setText(QString::fromLatin1(names[static_cast<int>(server_->state())]));
    presented_->setText(server_->peerFingerprint().isEmpty() ? tr("(none yet)")
                                                             : ServerClient::prettyFingerprint(server_->peerFingerprint()));
}
