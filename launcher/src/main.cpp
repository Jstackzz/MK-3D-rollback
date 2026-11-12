// MK3D Online launcher: ranked/casual matchmaking, lobbies, practice, leaderboard and the
// "build the game from your own disc" flow for the native Mortal Kombat ports.

#include <algorithm>
#include <QApplication>
#include <QTextStream>
#include <QDir>
#include <QEventLoop>
#include <QCommandLineParser>
#include <QLockFile>
#include <QMessageBox>
#include <QSslSocket>
#include <QStyleFactory>

#include "AppPaths.h"
#include "GameData.h"
#include "Jukebox.h"
#include "MainWindow.h"
#include "Style.h"
#include "WinUtil.h"

#ifdef MK3D_JUKEBOX
// --jukebox <game> [--jukebox-set <set>]: the plan from the saved settings (the set given first, when given), the banks
// made, the map written; what happened on stdout
static int jukeboxCommand(const QString& game, const QString& set) {
    const QList<jukebox::Soundtrack> library = jukebox::scanLibrary(jukebox::libraryFolder());
    jukebox::Config config = jukebox::loadConfig(game);
    if (!set.isEmpty()) {
        config.set = set;
        jukebox::saveConfig(game, config);
    }
    const jukebox::Plan plan = jukebox::resolve(game, config, library);
    QTextStream out(stdout);
    out << "music folder " << QDir::toNativeSeparators(jukebox::libraryFolder()) << ": " << library.size() << " soundtracks\n";
    out << "set " << config.set << ": " << jukebox::describeSet(config.set, library) << "\n";
    for (const jukebox::Pick& p : plan.picks)
        out << p.slot << " (" << (p.bank.isEmpty() ? QStringLiteral("no bank") : p.bank) << "): "
            << (p.file.isEmpty() ? QStringLiteral("the game's own") : p.soundtrack + ": " + p.title) << "\n";
    for (const QString& n : plan.notes) out << "note: " << n << "\n";
    out.flush();
    jukebox::Applier applier;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&applier, &jukebox::Applier::progress, [&](int done, int total, const QString& what) {
        out << "preparing " << done + 1 << "/" << total << ": " << what << "\n";
        out.flush();
    });
    QObject::connect(&applier, &jukebox::Applier::finished, [&](bool good, const QString& message) {
        ok = good;
        out << (good ? "done: " : "failed: ") << message << "\n";
        out.flush();
        loop.quit();
    });
    if (!applier.apply(game, plan)) return 2;
    loop.exec();
    out << "map " << QDir::toNativeSeparators(jukebox::mapFile(game)) << "\n";
    out.flush();
    return ok ? 0 : 1;
}
#endif

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("MK3D Online"));
    QApplication::setOrganizationName(QStringLiteral("MK3D"));
    QApplication::setApplicationVersion(QStringLiteral(MK3D_LAUNCHER_VERSION));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("MK3D Online launcher"));
    cli.addHelpOption();
    cli.addVersionOption();
    const QCommandLineOption configOpt(QStringLiteral("config"), QStringLiteral("Settings file to use."), QStringLiteral("ini"));
    const QCommandLineOption serverOpt(QStringLiteral("server"), QStringLiteral("Server address host:port."), QStringLiteral("address"));
    const QCommandLineOption fpOpt(QStringLiteral("fingerprint"), QStringLiteral("Pinned SHA-256 certificate fingerprint."), QStringLiteral("hex"));
    const QCommandLineOption userOpt(QStringLiteral("user"), QStringLiteral("Development: sign in as this account."), QStringLiteral("name"));
    const QCommandLineOption pwEnvOpt(QStringLiteral("password-env"),
                                      QStringLiteral("Development: environment variable holding the password (default MK3D_DEV_PASSWORD)."),
                                      QStringLiteral("var"));
    const QCommandLineOption regOpt(QStringLiteral("register"), QStringLiteral("Development: create the --user account if needed."));
    const QCommandLineOption queueOpt(QStringLiteral("queue"), QStringLiteral("Development: queue right after signing in."), QStringLiteral("casual|ranked"));
    const QCommandLineOption gameOpt(QStringLiteral("game"), QStringLiteral("Game to select: mkda, mkd, mka."), QStringLiteral("id"));
    const QCommandLineOption modeOpt(QStringLiteral("mode"), QStringLiteral("Mode for --queue."), QStringLiteral("id"));
    const QCommandLineOption charOpt(QStringLiteral("character"), QStringLiteral("Character for --queue."), QStringLiteral("id"));
    const QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Page to open."), QStringLiteral("name"));
    const QCommandLineOption localTestOpt(QStringLiteral("local-test"),
                                          QStringLiteral("Development: start the two-instance loopback test."));
    const QCommandLineOption extractOpt(QStringLiteral("extract"), QStringLiteral("Open Build game and extract this disc image."),
                                        QStringLiteral("image"));
    const QCommandLineOption acceptOpt(QStringLiteral("auto-accept"), QStringLiteral("Development: accept found matches after N seconds."),
                                       QStringLiteral("seconds"));
    const QCommandLineOption cancelOpt(
        QStringLiteral("cancel-search"),
        QStringLiteral("Development: cancel the --queue search N seconds after it starts, as CANCEL SEARCH does."),
        QStringLiteral("seconds"));
    const QCommandLineOption lobbyOpt(QStringLiteral("lobby"),
                                      QStringLiteral("Development: host a lobby (start once both are ready), join it "
                                                     "(ready at once) or watch it (as a spectator)."),
                                      QStringLiteral("host|join|watch"));
    const QCommandLineOption lobbyNameOpt(QStringLiteral("lobby-name"), QStringLiteral("Development: the lobby's name for --lobby."),
                                          QStringLiteral("name"));
    const QCommandLineOption upnpProbeOpt(QStringLiteral("upnp-probe"),
                                          QStringLiteral("Find the router (UPnP) and its public address; open no port."));
    const QCommandLineOption lobbyMatchesOpt(QStringLiteral("lobby-matches"),
                                             QStringLiteral("Development: matches to play in the --lobby (default 1)."),
                                             QStringLiteral("n"));
    const QCommandLineOption leaveLobbyOpt(
        QStringLiteral("leave-lobby"),
        QStringLiteral("Development: once the --lobby-matches are played, leave the lobby (its game then closes)."));
    const QCommandLineOption rematchesOpt(
        QStringLiteral("rematches"),
        QStringLiteral("Development: after a --queue casual match, choose a rematch this many times (a ranked set "
                       "plays on by itself)."),
        QStringLiteral("n"));
    const QCommandLineOption rematchChoiceOpt(
        QStringLiteral("rematch-choice"),
        QStringLiteral("Development: what each --rematches choice is, rematch (the same fighters) or select (both "
                       "pick again), comma-separated; the last one repeats (default rematch)."),
        QStringLiteral("choices"));
    const QCommandLineOption leaveSeriesOpt(
        QStringLiteral("leave-series"),
        QStringLiteral("Development: once the --rematches are chosen, leave the series at once instead of letting it run out."));
    const QCommandLineOption soloOpt(
        QStringLiteral("solo"),
        QStringLiteral("Development: with --queue, start single player first and search alongside it (standby)."));
    const QCommandLineOption deathTrapsOpt(
        QStringLiteral("death-traps"),
        QStringLiteral("The arenas' death traps in matches this player searches for or hosts, saved (on|off)."),
        QStringLiteral("on|off"));
    const QCommandLineOption practiceOpt(
        QStringLiteral("practice"),
        QStringLiteral("Practice while a Ranked or Kasual search waits: on or off (saved like the queue pages' box)."),
        QStringLiteral("on|off"));
    const QCommandLineOption searchAgainOpt(
        QStringLiteral("search-again"),
        QStringLiteral("Search again once a Ranked set or a Kasual series is over: on or off (saved like the queue "
                       "pages' box)."),
        QStringLiteral("on|off"));
    const QCommandLineOption updatedOpt(
        QStringLiteral("updated"),
        QStringLiteral("Started by an update of this version: waits for it to close, then deletes what it set aside."),
        QStringLiteral("version"));
    const QCommandLineOption jukeboxOpt(QStringLiteral("jukebox"),
                                        QStringLiteral("Development: prepare the jukebox's banks for this game from the saved "
                                                       "settings, print the plan, and exit."),
                                        QStringLiteral("game"));
    const QCommandLineOption jukeboxSetOpt(QStringLiteral("jukebox-set"),
                                           QStringLiteral("Development: with --jukebox, first set the game's music set (own, "
                                                          "preset:<soundtrack>, shuffle:<soundtrack>, shuffle:*, playlist:<n>)."),
                                           QStringLiteral("set"));
    const QCommandLineOption autoUpdateOpt(QStringLiteral("auto-update"),
                                           QStringLiteral("Development: install an update offered at once, without asking."));
    for (const auto* o : {&configOpt, &serverOpt, &fpOpt, &userOpt, &pwEnvOpt, &regOpt, &queueOpt, &gameOpt, &modeOpt,
                          &charOpt, &pageOpt, &acceptOpt, &cancelOpt, &localTestOpt, &extractOpt, &lobbyOpt, &lobbyNameOpt,
                          &lobbyMatchesOpt, &leaveLobbyOpt, &upnpProbeOpt, &rematchesOpt, &rematchChoiceOpt,
                          &leaveSeriesOpt, &soloOpt,
                          &practiceOpt, &searchAgainOpt, &deathTrapsOpt, &updatedOpt, &autoUpdateOpt, &jukeboxOpt,
                          &jukeboxSetOpt})
        cli.addOption(*o);
    cli.process(app);

    paths::init(cli.value(configOpt));
    winutil::stayBehind(cfg::devBackground());
    applog::write(QStringLiteral("launcher %1 starting, settings %2").arg(QStringLiteral(MK3D_LAUNCHER_VERSION), paths::configFile()));

    // not "<ini>.lock": QSettings locks that file itself while it syncs. Started by an update, the old launcher is
    // still closing: wait for it.
    QLockFile lock(paths::configFile() + QStringLiteral(".instance"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(cli.isSet(updatedOpt) ? 30000 : 200)) {
        QMessageBox::information(nullptr, QStringLiteral("MK3D Online"),
                                 QStringLiteral("MK3D Online is already running with these settings."));
        return 0;
    }

    QString error;
    if (!GameData::instance().load(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("MK3D Online"), error);
        return 1;
    }
#ifdef MK3D_JUKEBOX
    if (cli.isSet(jukeboxOpt)) return jukeboxCommand(cli.value(jukeboxOpt), cli.value(jukeboxSetOpt));
#endif
    if (QSslSocket::availableBackends().contains(QStringLiteral("schannel")))
        QSslSocket::setActiveBackend(QStringLiteral("schannel"));
    applog::write(QStringLiteral("tls backend %1, %2").arg(QSslSocket::activeBackend(), QSslSocket::sslLibraryVersionString()));

    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setPalette(style::palette());
    app.setStyleSheet(style::sheet());
    app.setWindowIcon(style::appIcon());

    LaunchOptions opts;
    opts.server = cli.value(serverOpt);
    opts.fingerprint = cli.value(fpOpt);
    opts.user = cli.value(userOpt);
    if (cli.isSet(pwEnvOpt)) opts.passwordEnv = cli.value(pwEnvOpt);
    opts.registerAccount = cli.isSet(regOpt);
    opts.queue = cli.value(queueOpt);
    opts.game = cli.value(gameOpt);
    opts.mode = cli.value(modeOpt);
    opts.character = cli.value(charOpt);
    opts.page = cli.value(pageOpt);
    opts.autoAcceptSeconds = cli.isSet(acceptOpt) ? cli.value(acceptOpt).toInt() : -1;
    opts.cancelSearchSeconds = cli.isSet(cancelOpt) ? std::max(0, cli.value(cancelOpt).toInt()) : -1;
    opts.localTest = cli.isSet(localTestOpt);
    opts.extractImage = cli.value(extractOpt);
    opts.lobby = cli.value(lobbyOpt);
    opts.lobbyName = cli.value(lobbyNameOpt);
    opts.lobbyMatches = cli.isSet(lobbyMatchesOpt) ? std::max(1, cli.value(lobbyMatchesOpt).toInt()) : 1;
    opts.leaveLobby = cli.isSet(leaveLobbyOpt);
    opts.rematches = cli.isSet(rematchesOpt) ? std::max(0, cli.value(rematchesOpt).toInt()) : 0;
    opts.rematchChoices = cli.value(rematchChoiceOpt).split(QLatin1Char(','), Qt::SkipEmptyParts);
    opts.leaveSeries = cli.isSet(leaveSeriesOpt);
    opts.solo = cli.isSet(soloOpt);
    opts.practice = cli.value(practiceOpt);
    opts.searchAgain = cli.value(searchAgainOpt);
    opts.deathTraps = cli.value(deathTrapsOpt);
    opts.upnpProbe = cli.isSet(upnpProbeOpt);
    opts.updatedFrom = cli.value(updatedOpt);
    opts.autoUpdate = cli.isSet(autoUpdateOpt);

    MainWindow window(opts);
    window.show();
    const int rc = app.exec();
    applog::write(QStringLiteral("launcher exiting (%1)").arg(rc));
    return rc;
}
