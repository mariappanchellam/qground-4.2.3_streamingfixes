# qgc-video-streamer

Crash-isolated UDP video streaming process for QGroundControl.

**No FFmpeg. No GStreamer.** Uses:
- **OpenH264** (Cisco, BSD-2) — H.264 decode + multi-resolution encode
- **SDL2** — local preview window
- **libcurl** — AWS S3 upload (SigV4 authentication built into curl ≥ 7.75)

## Pipeline

```
Drone camera (2K H.264/RTP on UDP)
    ↓
OpenH264 decoder  →  1920×1080 I420 frame
    ↓  (bilinear scaler, no external lib)
┌── 1920×1080  @ 4500 kbps ──┐
├── 1328×747   @ 2500 kbps   │  OpenH264 encoders
├──  568×320   @  800 kbps   │
├──  440×248   @  500 kbps   │
└──  256×144   @  200 kbps ──┘
    ↓  (minimal MPEG-TS muxer, no external lib)
HLS segments (2 s .ts files)
    ↓
AWS S3  →  master.m3u8 + per-level playlists
    ↓
CloudFront / any HLS player
```

Adaptive bitrate (ABR) selection is automatic: the process measures S3 upload
bandwidth and picks the highest quality level that can be sustained in real time.

## Build

```bash
# Dependencies
sudo apt-get install libopenh264-dev libsdl2-dev libcurl4-openssl-dev

# Build with g++
g++ -std=c++17 $(pkg-config --cflags sdl2 libcurl) \
    main.cpp -o qgc-video-streamer \
    $(pkg-config --libs sdl2 libcurl) -lopenh264 -lpthread

# Or with qmake (inside the QGC source tree)
cd tools/qgc-video-streamer
qmake && make -j$(nproc)
```

## Run

```bash
# Local preview only (no AWS upload):
./qgc-video-streamer -l -p 5600

# With AWS S3 upload:
export QGC_AWS_ACCESS_KEY_ID=AKIA...
export QGC_AWS_SECRET_ACCESS_KEY=...
export QGC_AWS_REGION=ap-south-1
export QGC_S3_BUCKET=my-drone-video
./qgc-video-streamer -p 5600 -n my-flight-001

# Force quality level (0=1080p … 4=144p):
./qgc-video-streamer -l -q 2
```

## Integration with QGC (QProcess)

QGC launches this as a QProcess child. If the streamer crashes, QGC does NOT
crash — it just restarts the child process.

See `src/VideoReceiver/UDPVideoStreamer.h` for the integration class.

## HLS output

After the process runs, AWS S3 will contain:

```
{stream_name}/master.m3u8           ← multi-variant master playlist
{stream_name}/1080p/stream.m3u8
{stream_name}/1080p/1080p_0001.ts
{stream_name}/747p/stream.m3u8
...
```

Play with: `ffplay https://your-bucket.s3.region.amazonaws.com/live/master.m3u8`
or any HLS-capable player (VLC, QuickTime, Safari, iOS, Android ExoPlayer, etc.)
