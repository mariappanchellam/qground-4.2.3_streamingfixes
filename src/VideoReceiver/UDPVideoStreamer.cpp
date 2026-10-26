#include "UDPVideoStreamer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>

UDPVideoStreamer::UDPVideoStreamer(QObject* parent)
    : QObject(parent)
{
    _restartTimer = new QTimer(this);
    _restartTimer->setSingleShot(true);
    connect(_restartTimer, &QTimer::timeout, this, &UDPVideoStreamer::_tryRestart);
}

UDPVideoStreamer::~UDPVideoStreamer() {
    stop();
}

void UDPVideoStreamer::start(const Config& cfg) {
    stop(); // stop any existing process
    _cfg = cfg;
    _restartCount = 0;

    QString exe = _streamerExePath();
    if (exe.isEmpty()) {
        emit error(QStringLiteral("qgc-video-streamer executable not found"));
        _setStatus(QStringLiteral("Error: streamer not found"));
        return;
    }

    _proc = new QProcess(this);
    connect(_proc, &QProcess::started,           this, &UDPVideoStreamer::_onStarted);
    connect(_proc, qOverload<int,QProcess::ExitStatus>(&QProcess::finished),
            this, &UDPVideoStreamer::_onFinished);
    connect(_proc, &QProcess::readyReadStandardError,
            this, &UDPVideoStreamer::_onReadyReadStdErr);

    // Pass AWS credentials as environment variables
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!cfg.awsAccessKey.isEmpty()) env.insert(QStringLiteral("QGC_AWS_ACCESS_KEY_ID"),  cfg.awsAccessKey);
    if (!cfg.awsSecretKey.isEmpty()) env.insert(QStringLiteral("QGC_AWS_SECRET_ACCESS_KEY"), cfg.awsSecretKey);
    if (!cfg.awsRegion.isEmpty())    env.insert(QStringLiteral("QGC_AWS_REGION"),          cfg.awsRegion);
    if (!cfg.s3Bucket.isEmpty())     env.insert(QStringLiteral("QGC_S3_BUCKET"),           cfg.s3Bucket);
    _proc->setProcessEnvironment(env);

    QStringList args = _buildArgs(cfg);
    qDebug() << "[UDPVideoStreamer] Starting:" << exe << args;
    _setStatus(QStringLiteral("Starting..."));
    _proc->start(exe, args);
}

void UDPVideoStreamer::stop() {
    _restartTimer->stop();
    _restartCount = 0;
    if (!_proc) return;
    if (_proc->state() != QProcess::NotRunning) {
        _proc->terminate();
        if (!_proc->waitForFinished(3000))
            _proc->kill();
    }
    _proc->deleteLater();
    _proc = nullptr;
    _setStatus(QStringLiteral("Stopped"));
    emit streamStopped();
    emit runningChanged();
}

bool UDPVideoStreamer::isRunning() const {
    return _proc && _proc->state() == QProcess::Running;
}

void UDPVideoStreamer::_onStarted() {
    qDebug() << "[UDPVideoStreamer] Process started, PID" << _proc->processId();
    _setStatus(QStringLiteral("Streaming"));
    emit runningChanged();

    // Publish the S3 master playlist URL if not local-only
    if (!_cfg.localOnly && !_cfg.s3Bucket.isEmpty()) {
        QString region = _cfg.awsRegion.isEmpty()
            ? qgetenv("QGC_AWS_REGION") : _cfg.awsRegion;
        if (region.isEmpty()) region = QStringLiteral("us-east-1");
        QString url = QStringLiteral("https://%1.s3.%2.amazonaws.com/%3/master.m3u8")
                        .arg(_cfg.s3Bucket, region, _cfg.streamName);
        emit streamStarted(url);
    }
}

void UDPVideoStreamer::_onFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    qDebug() << "[UDPVideoStreamer] Process finished, exit" << exitCode
             << (exitStatus==QProcess::CrashExit ? "(crash)":"(normal)");

    emit runningChanged();

    if (exitCode != 0 || exitStatus == QProcess::CrashExit) {
        if (_restartCount < MAX_RESTARTS) {
            _restartCount++;
            QString msg = QStringLiteral("Crashed (restart %1/%2 in %3 s)")
                            .arg(_restartCount).arg(MAX_RESTARTS)
                            .arg(RESTART_DELAY_MS/1000);
            _setStatus(msg);
            emit error(msg);
            _restartTimer->start(RESTART_DELAY_MS);
        } else {
            _setStatus(QStringLiteral("Failed (max restarts reached)"));
            emit error(QStringLiteral("Video streamer failed after %1 restarts").arg(MAX_RESTARTS));
        }
    } else {
        _setStatus(QStringLiteral("Stopped"));
        emit streamStopped();
    }
}

void UDPVideoStreamer::_onReadyReadStdErr() {
    if (!_proc) return;
    QByteArray data = _proc->readAllStandardError();
    // Forward streamer logs to QGC debug output
    for (auto& line : data.split('\n')) {
        if (!line.trimmed().isEmpty())
            qDebug() << "[streamer]" << line.trimmed();
    }
}

void UDPVideoStreamer::_tryRestart() {
    if (!_proc) return;
    qDebug() << "[UDPVideoStreamer] Restarting (attempt" << _restartCount << ")";
    // Re-use the same config
    Config cfg = _cfg;
    _proc->deleteLater();
    _proc = nullptr;
    start(cfg);
}

void UDPVideoStreamer::_setStatus(const QString& s) {
    if (_status == s) return;
    _status = s;
    emit statusChanged();
}

QString UDPVideoStreamer::_streamerExePath() const {
    // Look next to the QGC executable first, then in PATH
    QStringList candidates = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/qgc-video-streamer"),
        QStandardPaths::findExecutable(QStringLiteral("qgc-video-streamer")),
    };
    for (auto& c : candidates) {
        if (!c.isEmpty() && QFileInfo::exists(c)) return c;
    }
    return {};
}

QStringList UDPVideoStreamer::_buildArgs(const Config& cfg) const {
    QStringList args;
    args << QStringLiteral("-p") << QString::number(cfg.udpPort);
    args << QStringLiteral("-n") << cfg.streamName;
    args << QStringLiteral("-d") << cfg.hlsDir;
    args << QStringLiteral("-s") << QString::number(cfg.segmentSecs);
    if (cfg.localOnly) args << QStringLiteral("-l");
    if (cfg.forcedLevel >= 0)
        args << QStringLiteral("-q") << QString::number(cfg.forcedLevel);
    return args;
}
