/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTimer>

#include "QGCLoggingCategory.h"

Q_DECLARE_LOGGING_CATEGORY(SiyiCameraLog)

class QUdpSocket;

// Controls SIYI gimbal cameras (ZR10, ZR30, A8 mini, ...) directly over the SIYI gimbal SDK
// protocol on UDP port 37260 of the camera, independent of the flight controller.
class SiyiCameraController : public QObject
{
    Q_OBJECT

public:
    explicit SiyiCameraController(QObject* parent = nullptr);

    Q_PROPERTY(bool     enabled         READ enabled        WRITE setEnabled    NOTIFY enabledChanged)
    Q_PROPERTY(QString  host            READ host           WRITE setHost       NOTIFY hostChanged)
    Q_PROPERTY(bool     connected       READ connected                          NOTIFY connectedChanged)
    Q_PROPERTY(double   zoom            READ zoom                               NOTIFY zoomChanged)
    Q_PROPERTY(double   yaw             READ yaw                                NOTIFY attitudeChanged)
    Q_PROPERTY(double   pitch           READ pitch                              NOTIFY attitudeChanged)
    Q_PROPERTY(int      recordStatus    READ recordStatus                       NOTIFY configChanged)
    Q_PROPERTY(int      motionMode      READ motionMode                         NOTIFY configChanged)

    // Values reported by the camera (SIYI SDK "Acquire Gimbal Configuration Information", 0x0A)
    enum RecordStatus { RecordUnknown = -1, RecordOff = 0, RecordOn = 1, RecordNoCard = 2, RecordDataLoss = 3 };
    enum MotionMode   { ModeUnknown = -1, ModeLock = 0, ModeFollow = 1, ModeFpv = 2 };
    Q_ENUM(RecordStatus)
    Q_ENUM(MotionMode)

    static constexpr quint16 kPort = 37260;

    bool    enabled     () const { return _enabled; }
    QString host        () const { return _host; }
    bool    connected   () const { return _connected; }
    double  zoom        () const { return _zoom; }
    double  yaw         () const { return _yaw; }
    double  pitch       () const { return _pitch; }
    int     recordStatus() const { return _recordStatus; }
    int     motionMode  () const { return _motionMode; }

    void setEnabled (bool enabled);
    void setHost    (const QString& host);

    // Zoom keeps moving until zoomStop(); rotate() keeps the gimbal moving until stopRotation().
    Q_INVOKABLE void zoomIn         ();
    Q_INVOKABLE void zoomOut        ();
    Q_INVOKABLE void zoomStop       ();
    Q_INVOKABLE void zoomReset      ();
    Q_INVOKABLE void autoFocus      ();
    Q_INVOKABLE void rotate         (int yawSpeed, int pitchSpeed);   // -100..100, positive yaw = right, positive pitch = up
    Q_INVOKABLE void stopRotation   ();
    Q_INVOKABLE void center         ();
    Q_INVOKABLE void takePhoto      ();
    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void setMotionMode  (int mode);

    static QByteArray buildPacket(quint8 command, const QByteArray& data, quint16 sequence);
    static quint16    crc16      (const char* data, int length);

signals:
    void enabledChanged     ();
    void hostChanged        ();
    void connectedChanged   ();
    void zoomChanged        ();
    void attitudeChanged    ();
    void configChanged      ();
    void cameraMessage      (const QString& message);

private slots:
    void _readPendingDatagrams  ();
    void _poll                  ();
    void _repeatMotion          ();

private:
    void _send          (quint8 command, const QByteArray& data = QByteArray());
    void _handlePacket  (quint8 command, const QByteArray& data);
    void _setConnected  (bool connected);
    void _openSocket    ();
    void _closeSocket   ();

    bool            _enabled        = false;
    QString         _host;
    QHostAddress    _address;
    QUdpSocket*     _socket         = nullptr;
    quint16         _sequence       = 0;

    QTimer          _pollTimer;
    QTimer          _repeatTimer;
    QElapsedTimer   _lastReceive;
    int             _pollCount      = 0;

    int             _zoomDirection  = 0;
    int             _yawSpeed       = 0;
    int             _pitchSpeed     = 0;

    bool            _connected      = false;
    double          _zoom           = 0;
    double          _yaw            = 0;
    double          _pitch          = 0;
    int             _recordStatus   = RecordUnknown;
    int             _motionMode     = ModeUnknown;
};
