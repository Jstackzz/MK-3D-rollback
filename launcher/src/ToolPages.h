#pragma once
// "Build game" (disc image -> assets folder -> first launch) and Settings.

#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

#include "Widgets.h"
#include "Xiso.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QThread;
class GameProcess;
class ServerClient;
class UpnpMapper;

class BuildPage : public Page {
    Q_OBJECT
public:
    explicit BuildPage(QWidget* parent = nullptr);
    ~BuildPage() override;
    void setGame(const QString& gameId) override;
    void extractImage(const QString& path);  // fill in the image and start, as if Extract was clicked
    bool busy() const { return step_ != Step::Idle; }  // extracting a disc or building a game

signals:
    void busyChanged(bool busy);

private:
    enum class Step { Idle, Reading, Extracting, Compiling };
    struct WorkResult {
        bool ok = false;
        QString error;
        QString sha1;    // of the disc's default.xbe
        QByteArray xbe;  // the disc's default.xbe
    };
    void refreshInfo();
    void startExtract();
    void onDiscRead(const std::shared_ptr<xiso::Image>& disc, const WorkResult& r);
    void onExtracted(const WorkResult& r);
    void runWorker(std::function<void()> work, std::function<void()> done);
    void finishStep();
    void updateProgress();
    void compileFirstLaunch();
    void setBusy(bool busy);

    QLabel* gameTitle_;
    QLabel* folderInfo_;
    QLabel* assetsInfo_;
    QLabel* exeInfo_;
    QLineEdit* image_;
    QPushButton* browse_;
    QPushButton* extract_;
    QPushButton* compile_;
    QPushButton* cancel_;
    QProgressBar* progress_;
    LogView* log_;
    QThread* worker_ = nullptr;
    GameProcess* firstRun_ = nullptr;
    Step step_ = Step::Idle;
    std::atomic<bool> stop_{false};
    std::atomic<qint64> bytesDone_{0};
    std::atomic<int> filesDone_{0};
    qint64 totalBytes_ = 0;
    int totalFiles_ = 0;
    QTimer progressTimer_;
    QString outDir_;
    QString imagePath_;
};

class SettingsPage : public Page {
    Q_OBJECT
public:
    explicit SettingsPage(ServerClient* server, QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;
    void setUpnp(UpnpMapper* upnp);  // the router's state on the page; the game port's changes go to it

signals:
    void reconnectRequested();

private:
    void reloadServer();
    ServerClient* server_;
    QLineEdit* address_;
    QLineEdit* fingerprint_;
    QCheckBox* autoConnect_;
    QLabel* serverState_;
    QLabel* presented_;
    QLabel* routerState_;
    UpnpMapper* upnp_ = nullptr;
};
