# qgc-video-streamer — standalone H.264 UDP → multi-res HLS → AWS S3
# No FFmpeg, No GStreamer — uses OpenH264 (Cisco BSD-2) + SDL2 + libcurl

TEMPLATE = app
TARGET   = qgc-video-streamer
CONFIG  += console c++17
CONFIG  -= app_bundle

SOURCES += main.cpp

HEADERS += \
    rtp.h     \
    scaler.h  \
    codec.h   \
    ts_mux.h  \
    hls.h     \
    s3.h      \
    abr.h     \
    display.h

# OpenH264 (Cisco)
LIBS += -lopenh264

# SDL2 (already a QGC dependency on Linux)
PKGCONFIG += sdl2
CONFIG    += link_pkgconfig

# libcurl for AWS S3 upload
PKGCONFIG += libcurl

# Build flags
QMAKE_CXXFLAGS += -std=c++17
unix:QMAKE_LFLAGS += -Wl,-rpath,/usr/lib/x86_64-linux-gnu
