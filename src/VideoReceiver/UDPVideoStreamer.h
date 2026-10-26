/**
 * UDPVideoStreamer — crash-isolated video process launcher for QGroundControl.
 *
 * Spawns qgc-video-streamer as a QProcess child. If the child crashes, QGC
 * continues running and this class restarts the child automatically.
 *
 * No FFmpeg, no GStreamer linked into QGC for this path.
 * The child process (qgc-video-streamer) handles:
 *   - H.264/RTP reception over UDP
 *   - Multi-resolution OpenH264 encode (1080p/747p/320p/248p/144p)
 *   - HLS segmentation + AWS S3 upload
 *   - SDL2 local preview window (separate OS window)
 */
#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QUrl>

class UDPVideoStreamer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool  running  READ isRunning  NOTIFY runningChanged)
    Q_PROPERTY(QString status READ status     NOTIFY statusChanged)

public:
    explicit UDPVideoStreamer(QObject* parent = nullptr);
    ~UDPVideoStreamer() override;

    struct Config {
        quint16 udpPort        = 5600;
        QString streamName     = QStringLiteral("live");
        QString hlsDir         = QStringLiteral("/tmp/qgc-hls");
        int     segmentSecs    = 2;
        bool    localOnly      = false;   // skip S3 upload
        int     forcedLevel    = -1;      // -1=ABR auto; 0=1080p … 4=144p
        // AWS credentials (override env-var defaults)
        QString awsAccessKey;
        QString awsSecretKey;
        QString awsRegion;
        QString s3Bucket;
    };

    Q_INVOKABLE void start(const Config& cfg = {});
    Q_INVOKABLE void stop();

    bool    isRunning() const;
    QString status()    const { return _status; }

signals:
    void runningChanged();
    void statusChanged();
    void streamStarted(const QString& masterPlaylistUrl);
    void streamStopped();
    void error(const QString& msg);

private slots:
    void _onStarted();
    void _onFinished(int exitCode, QProcess::ExitStatus status);
    void _onReadyReadStdErr();
    void _tryRestart();

private:
    void _setStatus(const QString& s);
    QString _streamerExePath() const;
    QStringList _buildArgs(const Config& cfg) const;

    QProcess* _proc           = nullptr;
    QTimer*   _restartTimer   = nullptr;
    Config    _cfg;
    QString   _status;
    int       _restartCount   = 0;
    static const int MAX_RESTARTS = 5;
    static const int RESTART_DELAY_MS = 3000;
};
