/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "SiyiCameraController.h"

#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtEndian>

QGC_LOGGING_CATEGORY(SiyiCameraLog, "SiyiCameraLog")

// SIYI gimbal SDK frame: 55 66 | CTRL | data_len (u16 LE) | seq (u16 LE) | CMD_ID | data | CRC16 (u16 LE)
// CRC16 is CRC-16/XMODEM (poly 0x1021, init 0) over every byte before it. Layout and command ids as in
// ArduPilot's AP_Mount_Siyi driver; verified against the example packets in SIYI's SDK manual.
namespace {

constexpr quint8 kHeader1       = 0x55;
constexpr quint8 kHeader2       = 0x66;
constexpr int    kHeaderLength  = 8;
constexpr int    kCrcLength     = 2;

enum Command : quint8 {
    CmdAutoFocus        = 0x04,
    CmdManualZoom       = 0x05,
    CmdGimbalRotation   = 0x07,
    CmdCenter           = 0x08,
    CmdConfigInfo       = 0x0A,
    CmdFunctionFeedback = 0x0B,
    CmdPhoto            = 0x0C,
    CmdAttitude         = 0x0D,
    CmdAbsoluteZoom     = 0x0F,
};

// Data byte of CmdPhoto
enum PhotoFunction : quint8 {
    TakePicture         = 0,
    RecordVideoToggle   = 2,
    LockMode            = 3,
    FollowMode          = 4,
    FpvMode             = 5,
};

constexpr int kPollIntervalMs       = 500;
constexpr int kRepeatIntervalMs     = 200;
constexpr int kConnectionTimeoutMs  = 3000;

}

SiyiCameraController::SiyiCameraController(QObject* parent)
    : QObject(parent)
{
    _pollTimer.setInterval(kPollIntervalMs);
    connect(&_pollTimer, &QTimer::timeout, this, &SiyiCameraController::_poll);

    // Zoom and rotation commands are re-sent while active so a lost UDP packet cannot stall them
    _repeatTimer.setInterval(kRepeatIntervalMs);
    connect(&_repeatTimer, &QTimer::timeout, this, &SiyiCameraController::_repeatMotion);
}

quint16 SiyiCameraController::crc16(const char* data, int length)
{
    quint16 crc = 0;
    for (int i = 0; i < length; i++) {
        crc ^= static_cast<quint16>(static_cast<quint8>(data[i])) << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? static_cast<quint16>((crc << 1) ^ 0x1021) : static_cast<quint16>(crc << 1);
        }
    }
    return crc;
}

QByteArray SiyiCameraController::buildPacket(quint8 command, const QByteArray& data, quint16 sequence)
{
    QByteArray packet;
    packet.reserve(kHeaderLength + data.size() + kCrcLength);
    packet.append(static_cast<char>(kHeader1));
    packet.append(static_cast<char>(kHeader2));
    packet.append(static_cast<char>(1));                                  // CTRL: request ACK
    packet.append(static_cast<char>(data.size() & 0xFF));
    packet.append(static_cast<char>((data.size() >> 8) & 0xFF));
    packet.append(static_cast<char>(sequence & 0xFF));
    packet.append(static_cast<char>((sequence >> 8) & 0xFF));
    packet.append(static_cast<char>(command));
    packet.append(data);

    const quint16 crc = crc16(packet.constData(), packet.size());
    packet.append(static_cast<char>(crc & 0xFF));
    packet.append(static_cast<char>((crc >> 8) & 0xFF));
    return packet;
}

void SiyiCameraController::setEnabled(bool enabled)
{
    if (enabled == _enabled) {
        return;
    }
    _enabled = enabled;
    if (_enabled) {
        _openSocket();
    } else {
        _closeSocket();
    }
    emit enabledChanged();
}

void SiyiCameraController::setHost(const QString& host)
{
    const QString trimmed = host.trimmed();
    if (trimmed == _host) {
        return;
    }
    _host = trimmed;
    _address = QHostAddress(_host);
    if (_address.isNull()) {
        qCWarning(SiyiCameraLog) << "Invalid SIYI camera address" << _host;
    }
    _setConnected(false);
    emit hostChanged();
}

void SiyiCameraController::_openSocket()
{
    if (_socket == nullptr) {
        _socket = new QUdpSocket(this);
        connect(_socket, &QUdpSocket::readyRead, this, &SiyiCameraController::_readPendingDatagrams);
        if (!_socket->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
            qCWarning(SiyiCameraLog) << "Unable to open UDP socket:" << _socket->errorString();
        }
    }
    _pollCount = 0;
    _pollTimer.start();
    _poll();
}

void SiyiCameraController::_closeSocket()
{
    _pollTimer.stop();
    _repeatTimer.stop();
    _zoomDirection = _yawSpeed = _pitchSpeed = 0;
    if (_socket != nullptr) {
        _socket->deleteLater();
        _socket = nullptr;
    }
    _setConnected(false);
}

void SiyiCameraController::_send(quint8 command, const QByteArray& data)
{
    if (!_enabled || _socket == nullptr || _address.isNull()) {
        return;
    }
    _socket->writeDatagram(buildPacket(command, data, _sequence++), _address, kPort);
}

void SiyiCameraController::zoomIn()
{
    _zoomDirection = 1;
    _repeatMotion();
    _repeatTimer.start();
}

void SiyiCameraController::zoomOut()
{
    _zoomDirection = -1;
    _repeatMotion();
    _repeatTimer.start();
}

void SiyiCameraController::zoomStop()
{
    _zoomDirection = 0;
    // Sent twice: a lost stop packet would leave the lens zooming
    _send(CmdManualZoom, QByteArray(1, static_cast<char>(0)));
    _send(CmdManualZoom, QByteArray(1, static_cast<char>(0)));
    if (_yawSpeed == 0 && _pitchSpeed == 0) {
        _repeatTimer.stop();
    }
}

void SiyiCameraController::zoomReset()
{
    _zoomDirection = 0;
    const char oneTimes[] = { 1, 0 };   // integer part, tenths
    _send(CmdAbsoluteZoom, QByteArray(oneTimes, 2));
    _send(CmdManualZoom, QByteArray(1, static_cast<char>(0)));   // reply carries the new zoom ratio
}

void SiyiCameraController::autoFocus()
{
    _send(CmdAutoFocus, QByteArray(1, static_cast<char>(1)));
}

void SiyiCameraController::rotate(int yawSpeed, int pitchSpeed)
{
    _yawSpeed = qBound(-100, yawSpeed, 100);
    _pitchSpeed = qBound(-100, pitchSpeed, 100);
    if (_yawSpeed == 0 && _pitchSpeed == 0) {
        stopRotation();
        return;
    }
    _repeatMotion();
    _repeatTimer.start();
}

void SiyiCameraController::stopRotation()
{
    _yawSpeed = _pitchSpeed = 0;
    const QByteArray stop(2, static_cast<char>(0));
    // Sent twice: a lost stop packet would leave the gimbal turning
    _send(CmdGimbalRotation, stop);
    _send(CmdGimbalRotation, stop);
    if (_zoomDirection == 0) {
        _repeatTimer.stop();
    }
}

void SiyiCameraController::center()
{
    _send(CmdCenter, QByteArray(1, static_cast<char>(1)));
}

void SiyiCameraController::takePhoto()
{
    _send(CmdPhoto, QByteArray(1, static_cast<char>(TakePicture)));
}

void SiyiCameraController::toggleRecording()
{
    if (_recordStatus == RecordNoCard) {
        emit cameraMessage(tr("Camera has no SD card, cannot record"));
        return;
    }
    _send(CmdPhoto, QByteArray(1, static_cast<char>(RecordVideoToggle)));
    _send(CmdConfigInfo);
}

void SiyiCameraController::setMotionMode(int mode)
{
    quint8 function;
    switch (mode) {
    case ModeLock:   function = LockMode;   break;
    case ModeFollow: function = FollowMode; break;
    case ModeFpv:    function = FpvMode;    break;
    default:
        return;
    }
    _send(CmdPhoto, QByteArray(1, static_cast<char>(function)));
    _send(CmdConfigInfo);
}

void SiyiCameraController::_repeatMotion()
{
    if (_zoomDirection != 0) {
        // Zoom out is -1, sent as 0xFF
        _send(CmdManualZoom, QByteArray(1, static_cast<char>(_zoomDirection > 0 ? 1 : 0xFF)));
    }
    if (_yawSpeed != 0 || _pitchSpeed != 0) {
        const char speeds[] = { static_cast<char>(static_cast<qint8>(_yawSpeed)), static_cast<char>(static_cast<qint8>(_pitchSpeed)) };
        _send(CmdGimbalRotation, QByteArray(speeds, 2));
    }
    if (_zoomDirection == 0 && _yawSpeed == 0 && _pitchSpeed == 0) {
        _repeatTimer.stop();
    }
}

void SiyiCameraController::_poll()
{
    _send(CmdAttitude);

    // Every 2 s: recording state / gimbal mode, and the current zoom ratio (a zoom "stop" is answered
    // with the ratio; only sent while not zooming so it cannot interrupt the user)
    if (_pollCount % 4 == 0) {
        _send(CmdConfigInfo);
        if (_zoomDirection == 0) {
            _send(CmdManualZoom, QByteArray(1, static_cast<char>(0)));
        }
    }
    _pollCount++;

    if (_connected && (!_lastReceive.isValid() || _lastReceive.elapsed() > kConnectionTimeoutMs)) {
        _setConnected(false);
    }
}

void SiyiCameraController::_readPendingDatagrams()
{
    while (_socket != nullptr && _socket->hasPendingDatagrams()) {
        const QByteArray datagram = _socket->receiveDatagram().data();

        // A datagram can hold one or more frames
        int offset = 0;
        while (datagram.size() - offset >= kHeaderLength + kCrcLength) {
            const char* frame = datagram.constData() + offset;
            if (static_cast<quint8>(frame[0]) != kHeader1 || static_cast<quint8>(frame[1]) != kHeader2) {
                offset++;
                continue;
            }

            const int dataLength = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(frame + 3));
            const int frameLength = kHeaderLength + dataLength + kCrcLength;
            if (datagram.size() - offset < frameLength) {
                break;
            }

            const quint16 expected = crc16(frame, kHeaderLength + dataLength);
            const quint16 received = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(frame + kHeaderLength + dataLength));
            if (expected == received) {
                _lastReceive.start();
                _setConnected(true);
                _handlePacket(static_cast<quint8>(frame[7]), QByteArray(frame + kHeaderLength, dataLength));
                offset += frameLength;
            } else {
                qCDebug(SiyiCameraLog) << "CRC mismatch, dropping frame";
                offset++;
            }
        }
    }
}

void SiyiCameraController::_handlePacket(quint8 command, const QByteArray& data)
{
    const uchar* d = reinterpret_cast<const uchar*>(data.constData());

    switch (command) {
    case CmdManualZoom:
        if (data.size() >= 2) {
            const double zoom = qFromLittleEndian<quint16>(d) / 10.0;
            if (zoom != _zoom) {
                _zoom = zoom;
                emit zoomChanged();
            }
        }
        break;
    case CmdAttitude:
        if (data.size() >= 6) {
            // The camera reports yaw positive to the left; flip it so positive matches rotate() (right)
            _yaw = -qFromLittleEndian<qint16>(d) / 10.0;
            _pitch = qFromLittleEndian<qint16>(d + 2) / 10.0;
            emit attitudeChanged();
        }
        break;
    case CmdConfigInfo:
        if (data.size() >= 5) {
            const int recordStatus = d[3];
            const int motionMode = d[4];
            if (recordStatus != _recordStatus || motionMode != _motionMode) {
                _recordStatus = recordStatus;
                _motionMode = motionMode;
                emit configChanged();
            }
        }
        break;
    case CmdFunctionFeedback:
        if (data.size() >= 1) {
            if (d[0] == 1) {
                emit cameraMessage(tr("Camera failed to take the photo"));
            } else if (d[0] == 4) {
                emit cameraMessage(tr("Camera failed to record video"));
            }
        }
        break;
    default:
        break;
    }
}

void SiyiCameraController::_setConnected(bool connected)
{
    if (connected != _connected) {
        _connected = connected;
        qCDebug(SiyiCameraLog) << "SIYI camera" << _host << (connected ? "connected" : "not responding");
        emit connectedChanged();
    }
}
