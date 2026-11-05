/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick          2.12
import QtQuick.Layouts  1.12

import QGroundControl               1.0
import QGroundControl.ScreenTools   1.0
import QGroundControl.Controls      1.0
import QGroundControl.Palette       1.0

// Zoom, gimbal, photo and record controls for a SIYI gimbal camera (ZR10, ZR30, A8 mini).
// Tap the header to collapse the panel to one line.
Rectangle {
    id:         root
    height:     mainLayout.height + (_margins * 2)
    color:      "#80000000"
    radius:     _margins
    visible:    _camera && _camera.enabled

    property var    _camera:        QGroundControl.videoManager.siyiCamera
    property real   _margins:       ScreenTools.defaultFontPixelHeight / 2
    property real   _buttonSize:    ScreenTools.defaultFontPixelHeight * 1.8
    property real   _gap:           ScreenTools.defaultFontPixelHeight / 4
    property int    _speed:         60
    property bool   _expanded:      true
    property bool   _recording:     _camera && _camera.recordStatus === SiyiCameraController.RecordOn

    QGCPalette { id: qgcPal; colorGroupEnabled: enabled }

    ColumnLayout {
        id:                         mainLayout
        anchors.margins:            _margins
        anchors.top:                parent.top
        anchors.horizontalCenter:   parent.horizontalCenter
        spacing:                    _gap * 2

        QGCLabel {
            Layout.alignment:   Qt.AlignHCenter
            text:               (_camera.connected ?
                                     qsTr("ZR10 %1x  P %2° Y %3°").arg(_camera.zoom.toFixed(1)).arg(_camera.pitch.toFixed(0)).arg(_camera.yaw.toFixed(0)) :
                                     qsTr("ZR10 not connected")) + (_expanded ? "  ▴" : "  ▾")
            color:              _camera.connected ? qgcPal.text : qgcPal.colorOrange
            font.pointSize:     ScreenTools.smallFontPointSize
            font.bold:          true

            QGCMouseArea {
                fillItem:   parent
                onClicked:  _expanded = !_expanded
            }
        }

        // Gimbal arrows (hold to turn, centre button re-centres) and zoom (hold + / -, 1x resets)
        RowLayout {
            Layout.alignment:   Qt.AlignHCenter
            spacing:            _gap * 3
            visible:            _expanded

            GridLayout {
                columns:        3
                rowSpacing:     _gap
                columnSpacing:  _gap

                Item { width: _buttonSize; height: _buttonSize }
                QGCButton {
                    Layout.preferredWidth:  _buttonSize
                    Layout.preferredHeight: _buttonSize
                    text:                   "▲"
                    onPressed:              _camera.rotate(0, _speed)
                    onReleased:             _camera.stopRotation()
                    onCanceled:             _camera.stopRotation()
                }
                Item { width: _buttonSize; height: _buttonSize }

                QGCButton {
                    Layout.preferredWidth:  _buttonSize
                    Layout.preferredHeight: _buttonSize
                    text:                   "◀"
                    onPressed:              _camera.rotate(-_speed, 0)
                    onReleased:             _camera.stopRotation()
                    onCanceled:             _camera.stopRotation()
                }
                QGCButton {
                    Layout.preferredWidth:  _buttonSize
                    Layout.preferredHeight: _buttonSize
                    text:                   "◎"
                    onClicked:              _camera.center()
                }
                QGCButton {
                    Layout.preferredWidth:  _buttonSize
                    Layout.preferredHeight: _buttonSize
                    text:                   "▶"
                    onPressed:              _camera.rotate(_speed, 0)
                    onReleased:             _camera.stopRotation()
                    onCanceled:             _camera.stopRotation()
                }

                Item { width: _buttonSize; height: _buttonSize }
                QGCButton {
                    Layout.preferredWidth:  _buttonSize
                    Layout.preferredHeight: _buttonSize
                    text:                   "▼"
                    onPressed:              _camera.rotate(0, -_speed)
                    onReleased:             _camera.stopRotation()
                    onCanceled:             _camera.stopRotation()
                }
                Item { width: _buttonSize; height: _buttonSize }
            }

            ColumnLayout {
                spacing: _gap

                QGCButton {
                    Layout.preferredWidth:  _buttonSize
                    Layout.preferredHeight: _buttonSize
                    text:                   "+"
                    onPressed:              _camera.zoomIn()
                    onReleased:             _camera.zoomStop()
                    onCanceled:             _camera.zoomStop()
                }
                QGCButton {
                    Layout.preferredWidth:  _buttonSize
                    Layout.preferredHeight: _buttonSize
                    text:                   "1x"
                    onClicked:              _camera.zoomReset()
                }
                QGCButton {
                    Layout.preferredWidth:  _buttonSize
                    Layout.preferredHeight: _buttonSize
                    text:                   "−"
                    onPressed:              _camera.zoomOut()
                    onReleased:             _camera.zoomStop()
                    onCanceled:             _camera.zoomStop()
                }
            }
        }

        // Photo / record go to the camera's own SD card; AF triggers auto focus
        RowLayout {
            Layout.alignment:   Qt.AlignHCenter
            spacing:            _gap
            visible:            _expanded

            QGCButton {
                Layout.preferredHeight: _buttonSize
                text:                   qsTr("Photo")
                onClicked:              _camera.takePhoto()
            }
            QGCButton {
                Layout.preferredHeight: _buttonSize
                text:                   _recording ? qsTr("Stop") : qsTr("Rec")
                onClicked:              _camera.toggleRecording()

                Rectangle {
                    anchors.fill:   parent
                    color:          "transparent"
                    border.color:   qgcPal.colorRed
                    border.width:   2
                    visible:        _recording
                }
            }
            QGCButton {
                Layout.preferredHeight: _buttonSize
                text:                   qsTr("AF")
                onClicked:              _camera.autoFocus()
            }
        }

        QGCLabel {
            Layout.alignment:   Qt.AlignHCenter
            text:               qsTr("No SD card in camera")
            color:              qgcPal.colorOrange
            font.pointSize:     ScreenTools.smallFontPointSize
            visible:            _expanded && _camera.recordStatus === SiyiCameraController.RecordNoCard
        }

        QGCComboBox {
            Layout.alignment:       Qt.AlignHCenter
            Layout.preferredWidth:  _buttonSize * 4
            model:                  [ qsTr("Lock"), qsTr("Follow"), qsTr("FPV") ]
            currentIndex:           _camera.motionMode >= 0 ? _camera.motionMode : 1
            visible:                _expanded
            onActivated:            _camera.setMotionMode(index)
        }
    }
}
