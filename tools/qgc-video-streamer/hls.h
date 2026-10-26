// HLS segment manager — writes .ts segment files and updates .m3u8 manifest.
// Segments are collected in a temp dir, then uploaded by the S3 layer.
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "ts_mux.h"

struct HLSConfig {
    std::string outdir       = "/tmp/hls";   // local staging directory
    std::string stream_name  = "stream";     // prefix for filenames
    int   segment_duration_s = 2;            // target segment length
    int   window_size        = 5;            // segments to keep in live playlist
    float fps                = 30.0f;
};

struct Segment {
    std::string filename;    // local path of .ts file
    std::string key;         // S3 key (relative path for upload)
    double      duration_s;
    bool        uploaded = false;
};

class HLSWriter {
    HLSConfig cfg;
    TSMuxer   mux;
    int       seg_index      = 0;
    int       frames_in_seg  = 0;
    int       target_frames;
    bool      seg_open       = false;
    FILE*     seg_fp         = nullptr;
    std::string seg_path;
    std::string seg_key;
    std::deque<Segment> window;

    std::string seg_filename(int idx) const {
        char buf[128];
        snprintf(buf,sizeof buf,"%s_%04d.ts", cfg.stream_name.c_str(), idx);
        return buf;
    }

    void open_segment() {
        seg_path = cfg.outdir + "/" + seg_filename(seg_index);
        seg_key  = cfg.stream_name + "/" + seg_filename(seg_index);
        seg_fp   = fopen(seg_path.c_str(), "wb");
        frames_in_seg = 0;
        seg_open = true;
        mux.reset_clock();
        // Write PAT+PMT at segment start (required for HLS)
        auto tables = mux.make_pat_pmt();
        fwrite(tables.data(), 1, tables.size(), seg_fp);
    }

    Segment close_segment() {
        fclose(seg_fp); seg_fp=nullptr; seg_open=false;
        Segment s;
        s.filename  = seg_path;
        s.key       = seg_key;
        s.duration_s = (double)frames_in_seg / cfg.fps;
        seg_index++;
        return s;
    }
public:
    explicit HLSWriter(const HLSConfig& c) : cfg(c), mux(c.fps) {
        target_frames = (int)(c.fps * c.segment_duration_s + 0.5f);
        mkdir(c.outdir.c_str(), 0755);
    }
    ~HLSWriter() { if(seg_fp) fclose(seg_fp); }

    // Returns a finished Segment when one is ready (otherwise nullopt).
    // Call with each encoded H.264 frame.
    std::vector<Segment> feed(const uint8_t* nal_data, int nal_len, bool is_keyframe) {
        std::vector<Segment> done;
        // Start a new segment on IDR (or first frame)
        if (!seg_open) {
            if (!is_keyframe) return done; // wait for IDR
            open_segment();
        }
        // Close segment on IDR boundary after target length
        if (is_keyframe && frames_in_seg >= target_frames) {
            done.push_back(close_segment());
            // Write PAT+PMT at start of new segment for the IDR that caused the cut
            open_segment();
        }
        // Write NAL to current segment
        auto ts = mux.write_nal(nal_data, nal_len, is_keyframe);
        fwrite(ts.data(), 1, ts.size(), seg_fp);
        frames_in_seg++;
        return done;
    }

    // Force-close the current segment (e.g. at shutdown).
    Segment flush() {
        if (!seg_open) return {};
        return close_segment();
    }

    // Write/update the HLS playlist for this quality level.
    void write_playlist(const std::string& playlist_path,
                        const std::deque<Segment>& segs,
                        bool ended=false) const {
        std::ofstream f(playlist_path);
        f << "#EXTM3U\n";
        f << "#EXT-X-VERSION:3\n";
        f << "#EXT-X-TARGETDURATION:" << cfg.segment_duration_s+1 << "\n";
        if (!segs.empty())
            f << "#EXT-X-MEDIA-SEQUENCE:" << (seg_index - (int)segs.size()) << "\n";
        for (auto& s : segs) {
            f << "#EXTINF:" << s.duration_s << ",\n";
            f << s.key << "\n";
        }
        if (ended) f << "#EXT-X-ENDLIST\n";
    }

    int currentSegIndex() const { return seg_index; }
};

// ──────────────────────────────────────────────
// Multi-variant master playlist (MASTER.m3u8)
// ──────────────────────────────────────────────
struct VariantInfo {
    int width, height, bandwidth_bps;
    std::string playlist_uri; // e.g. "720p/stream.m3u8"
};

static inline void write_master_playlist(const std::string& path,
                                          const std::vector<VariantInfo>& variants) {
    std::ofstream f(path);
    f << "#EXTM3U\n#EXT-X-VERSION:3\n";
    for (auto& v : variants) {
        f << "#EXT-X-STREAM-INF:BANDWIDTH=" << v.bandwidth_bps
          << ",RESOLUTION=" << v.width << "x" << v.height
          << ",CODECS=\"avc1.640028\"\n";
        f << v.playlist_uri << "\n";
    }
}
