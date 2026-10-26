/*
 * qgc-video-streamer — Crash-isolated H.264/H.265 UDP video pipeline
 *
 * Architecture (separate from QGC main process):
 *
 *   Drone camera (2K, H.264/RTP)
 *        │ UDP port 5600
 *        ▼
 *   RTP receiver → NAL assembler
 *        │ Annex-B NAL units
 *        ▼
 *   OpenH264 decoder (2K frame)
 *        │ I420 YUV planes
 *        ▼
 *   Bilinear scaler × N resolutions
 *   [1920×1080, 1328×747, 568×320, 440×248, 256×144]
 *        │ per-resolution I420
 *        ▼
 *   OpenH264 encoder × N (one per resolution)
 *        │ Annex-B NAL units per level
 *        ▼
 *   HLS segmenter → MPEG-TS files (~2 s)
 *        │ .ts segments + .m3u8 playlists
 *        ▼
 *   AWS S3 uploader (libcurl, SigV4 auth)
 *        │
 *        ▼ CloudFront CDN → HLS player (any device)
 *
 *   SDL2 local preview window (highest quality level)
 *
 * Required environment variables for AWS upload:
 *   QGC_AWS_ACCESS_KEY_ID, QGC_AWS_SECRET_ACCESS_KEY,
 *   QGC_AWS_REGION (default us-east-1), QGC_S3_BUCKET
 *
 * Usage:
 *   qgc-video-streamer [options]
 *   -p <port>       UDP video port (default 5600)
 *   -w <WxH>        Preview window size (default 1280x720)
 *   -r              Raw Annex-B mode (no RTP header)
 *   -n <name>       Stream name / S3 prefix (default "live")
 *   -d <dir>        Local HLS staging dir (default /tmp/qgc-hls)
 *   -s <seconds>    HLS segment duration (default 2)
 *   -q <idx>        Force quality level 0..4 (0=1080p, 4=144p)
 *   -l              Local preview only — do NOT upload to S3
 *   -h              Help
 *
 * If the process crashes, QGC (which launched it via QProcess) survives
 * and can restart it automatically.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <signal.h>

#include "rtp.h"
#include "scaler.h"
#include "codec.h"
#include "ts_mux.h"
#include "hls.h"
#include "s3.h"
#include "abr.h"
#include "display.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

// ──────────────────────────────────────────────
// Config
// ──────────────────────────────────────────────
struct Config {
    uint16_t    port          = 5600;
    int         win_w         = 1280;
    int         win_h         = 720;
    bool        raw_mode      = false;   // skip RTP header
    std::string stream_name   = "live";
    std::string hls_dir       = "/tmp/qgc-hls";
    int         seg_duration  = 2;
    int         forced_level  = -1;      // -1 = ABR auto
    bool        local_only    = false;   // skip S3 upload
    float       fps           = 30.0f;
};

static void usage(const char* p) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -p <port>      UDP video port (default 5600)\n"
        "  -w <WxH>       Preview window (default 1280x720)\n"
        "  -r             Raw Annex-B (no RTP wrapper)\n"
        "  -n <name>      Stream name / S3 prefix (default 'live')\n"
        "  -d <dir>       HLS staging directory (default /tmp/qgc-hls)\n"
        "  -s <sec>       Segment duration in seconds (default 2)\n"
        "  -q <0-4>       Force quality: 0=1080p 1=747p 2=320p 3=248p 4=144p\n"
        "  -l             Local preview only, skip S3\n"
        "  -h             This help\n"
        "\nAWS env vars: QGC_AWS_ACCESS_KEY_ID, QGC_AWS_SECRET_ACCESS_KEY,\n"
        "               QGC_AWS_REGION, QGC_S3_BUCKET\n", p);
}

// ──────────────────────────────────────────────
// Per-quality encoding pipeline
// ──────────────────────────────────────────────
struct EncPipeline {
    const QualityLevel* level = nullptr;
    H264Encoder  encoder;
    HLSWriter*   hls    = nullptr;
    bool         ready  = false;

    bool init(const QualityLevel& ql, const HLSConfig& hcfg) {
        level = &ql;
        if (!encoder.init(ql.width, ql.height, ql.bitrate_kbps)) {
            fprintf(stderr,"[enc] Failed to init encoder for %s\n",ql.name.c_str());
            return false;
        }
        HLSConfig cfg = hcfg;
        cfg.outdir       = hcfg.outdir + "/" + ql.name;
        cfg.stream_name  = ql.name;
        hls = new HLSWriter(cfg);
        ready = true;
        return true;
    }
    ~EncPipeline() { delete hls; }
};

// ──────────────────────────────────────────────
// Write the master M3U8 and upload it
// ──────────────────────────────────────────────
static void update_master_playlist(const std::string& dir,
                                    const std::string& stream_name,
                                    const std::vector<QualityLevel>& levels,
                                    S3Uploader& s3) {
    std::vector<VariantInfo> vi;
    for (auto& l : levels) {
        vi.push_back({l.width, l.height, l.bitrate_kbps*1000,
                      stream_name+"/"+l.name+"/stream.m3u8"});
    }
    std::string master_path = dir + "/master.m3u8";
    write_master_playlist(master_path, vi);
    if (s3.ready())
        s3.upload_file(master_path, stream_name+"/master.m3u8",
                       "application/vnd.apple.mpegurl");
}

// ──────────────────────────────────────────────
// main
// ──────────────────────────────────────────────
int main(int argc, char* argv[]) {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    Config cfg;
    for (int i=1; i<argc; i++) {
        if      (!strcmp(argv[i],"-p") && i+1<argc) cfg.port=(uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i],"-w") && i+1<argc) sscanf(argv[++i],"%dx%d",&cfg.win_w,&cfg.win_h);
        else if (!strcmp(argv[i],"-r")) cfg.raw_mode=true;
        else if (!strcmp(argv[i],"-n") && i+1<argc) cfg.stream_name=argv[++i];
        else if (!strcmp(argv[i],"-d") && i+1<argc) cfg.hls_dir=argv[++i];
        else if (!strcmp(argv[i],"-s") && i+1<argc) cfg.seg_duration=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-q") && i+1<argc) cfg.forced_level=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-l")) cfg.local_only=true;
        else if (!strcmp(argv[i],"-h")){ usage(argv[0]); return 0; }
    }

    fprintf(stderr,"[streamer] Starting — UDP port %d, stream '%s'\n",
            cfg.port, cfg.stream_name.c_str());
    if (cfg.local_only)
        fprintf(stderr,"[streamer] Local-only mode (no S3 upload)\n");

    // ── UDP socket
    UDPSock sock;
    if (!sock.open(cfg.port)) {
        fprintf(stderr,"[streamer] Cannot bind UDP port %d\n",cfg.port); return 1;
    }

    // ── Decoder
    H264Decoder decoder;
    if (!decoder.ready()) {
        fprintf(stderr,"[streamer] OpenH264 decoder init failed\n"); return 1;
    }
    fprintf(stderr,"[streamer] OpenH264 decoder ready\n");

    // ── Quality ladder + encoders
    auto ladder = default_quality_ladder();
    ABRController abr(ladder);
    if (cfg.forced_level >= 0) abr.force_level(cfg.forced_level);

    HLSConfig hcfg;
    hcfg.outdir              = cfg.hls_dir;
    hcfg.segment_duration_s  = cfg.seg_duration;
    hcfg.fps                 = cfg.fps;

    std::vector<std::unique_ptr<EncPipeline>> pipelines;
    for (auto& level : ladder) {
        auto p = std::make_unique<EncPipeline>();
        if (p->init(level, hcfg))
            pipelines.push_back(std::move(p));
        else
            fprintf(stderr,"[streamer] Skipping %s encoder (init failed)\n",level.name.c_str());
    }
    if (pipelines.empty()) { fprintf(stderr,"[streamer] No encoders available\n"); return 1; }
    fprintf(stderr,"[streamer] %zu quality levels active\n", pipelines.size());

    // ── S3
    S3Config s3cfg = cfg.local_only ? S3Config{} : S3Config::from_env();
    S3Uploader s3(s3cfg);
    if (!cfg.local_only && !s3.ready())
        fprintf(stderr,"[streamer] S3 upload disabled (missing credentials)\n");
    if (s3.ready()) {
        fprintf(stderr,"[streamer] S3 bucket: %s/%s\n",
                s3cfg.bucket.c_str(), cfg.stream_name.c_str());
        // Measure bandwidth first
        fprintf(stderr,"[streamer] Measuring upload bandwidth...\n");
        double bw = s3.measure_bandwidth_kbps();
        if (bw>0) {
            fprintf(stderr,"[streamer] Upload BW: %.0f kbps\n",bw);
            abr.update_bandwidth(bw);
        }
        update_master_playlist(cfg.hls_dir, cfg.stream_name, ladder, s3);
    }

    // ── Display
    std::string win_title = "QGC Video — " + cfg.stream_name;
    VideoDisplay display(win_title.c_str(), cfg.win_w, cfg.win_h);
    if (!display.valid())
        fprintf(stderr,"[streamer] SDL2 display not available (continuing without preview)\n");

    // ── Main loop
    NalAssembler asm_;
    std::vector<uint8_t> pkt(65535);
    std::vector<YUVFrame> scaled(pipelines.size());

    fprintf(stderr,"[streamer] Receiving H.264 on UDP port %d...\n", cfg.port);

    while (!g_stop) {
        if (!display.poll_events()) break;

        int n = sock.recv(pkt.data(), (int)pkt.size(), 20);
        if (n <= 0) continue;

        const uint8_t* payload = pkt.data();
        int            plen    = n;
        bool           marker  = false;

        if (!cfg.raw_mode) {
            if (n < 12) continue;
            const RTPHdr* hdr = (const RTPHdr*)pkt.data();
            if (hdr->v != 2) continue;
            int hdr_sz = 12 + hdr->cc*4;
            if (hdr->x) {
                if (n < hdr_sz+4) continue;
                uint16_t ext_len = (uint16_t(pkt[hdr_sz+2])<<8)|pkt[hdr_sz+3];
                hdr_sz += 4 + ext_len*4;
            }
            if (n < hdr_sz) continue;
            marker  = hdr->m;
            payload = pkt.data()+hdr_sz;
            plen    = n-hdr_sz;
        }

        // Assemble NAL units
        auto nals = asm_.feed(payload, plen, marker);
        for (auto& nal : nals) {
            // Decode to full-resolution I420
            uint8_t* yuv[3]{}; int strides[2]{}; int dw,dh;
            if (!decoder.decode(nal.data(),(int)nal.size(),yuv,strides,dw,dh)) continue;

            // Show native resolution in preview
            if (display.valid())
                display.present(yuv, strides, dw, dh);

            // Scale + encode for each quality level
            for (size_t i=0; i<pipelines.size(); i++) {
                auto& pip = *pipelines[i];
                if (!pip.ready) continue;
                int tw=pip.level->width, th=pip.level->height;
                scale_yuv420(yuv[0],strides[0], yuv[1],strides[1], yuv[2],
                             dw,dh, scaled[i], tw, th);

                EncodeResult enc = pip.encoder.encode(scaled[i]);
                if (enc.data.empty()) continue;

                // Feed into HLS segmenter
                auto done_segs = pip.hls->feed(enc.data.data(),(int)enc.data.size(),
                                                enc.is_keyframe);
                // Upload completed segments
                for (auto& seg : done_segs) {
                    if (!cfg.local_only && s3.ready()) {
                        auto t0=std::chrono::steady_clock::now();
                        int rc=s3.upload_file(seg.filename, seg.key);
                        auto t1=std::chrono::steady_clock::now();
                        if (rc>=200&&rc<300) {
                            double elapsed=std::chrono::duration<double>(t1-t0).count();
                            struct stat st{}; stat(seg.filename.c_str(),&st);
                            abr.record_upload((int)st.st_size, elapsed);
                        }
                    }
                    // Write updated playlist for this quality
                    // (simplified: just write a rolling playlist file)
                    std::string playlist_path =
                        cfg.hls_dir+"/"+pip.level->name+"/stream.m3u8";
                    // upload playlist
                    if (!cfg.local_only && s3.ready()) {
                        s3.upload_file(playlist_path,
                                       cfg.stream_name+"/"+pip.level->name+"/stream.m3u8",
                                       "application/vnd.apple.mpegurl");
                    }
                }
            }
        }
    }

    // Flush any open segments on shutdown
    for (auto& pip : pipelines) {
        if (!pip->ready) continue;
        Segment last = pip->hls->flush();
        if (!last.filename.empty() && !cfg.local_only && s3.ready()) {
            s3.upload_file(last.filename, last.key);
            std::string pp = cfg.hls_dir+"/"+pip->level->name+"/stream.m3u8";
            s3.upload_file(pp, cfg.stream_name+"/"+pip->level->name+"/stream.m3u8",
                           "application/vnd.apple.mpegurl");
        }
    }

    fprintf(stderr,"[streamer] Stopped cleanly.\n");
    return 0;
}
