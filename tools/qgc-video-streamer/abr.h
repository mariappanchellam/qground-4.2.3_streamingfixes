// Adaptive Bitrate controller.
// Selects the highest quality level that the current upload bandwidth supports.
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <deque>
#include <numeric>

struct QualityLevel {
    int    width, height;       // resolution (16:9)
    int    bitrate_kbps;        // target encode bitrate
    int    required_kbps;       // minimum upload bandwidth needed (1.5x encode)
    std::string name;           // e.g. "1080p"
};

// Default quality ladder matching user's request:
//   Input 2K (2048×1152) → 1080p / 747p / 320p / 248p / 144p
static inline std::vector<QualityLevel> default_quality_ladder() {
    return {
        {1920,1080, 4500, 6750, "1080p"},
        {1328, 747, 2500, 3750,  "747p"},
        { 568, 320,  800, 1200,  "320p"},
        { 440, 248,  500,  750,  "248p"},
        { 256, 144,  200,  300,  "144p"},
    };
}

class ABRController {
    std::vector<QualityLevel> ladder;
    int   current_level  = 0;   // index into ladder (0=highest)
    double bw_kbps       = -1;  // last measured bandwidth

    // Smoothed bandwidth history (EWMA)
    double ewma_bw = -1;
    static constexpr double ALPHA = 0.3; // EWMA weight for new sample

    // Upload timing measurement
    struct UploadSample {
        int    bytes;
        double elapsed_s;
    };
    std::deque<UploadSample> samples;
    static const int MAX_SAMPLES = 10;
public:
    explicit ABRController(std::vector<QualityLevel> ladder_={})
        : ladder(ladder_.empty() ? default_quality_ladder() : std::move(ladder_))
    {
        // Start at the lowest quality until we have a bandwidth estimate
        current_level = (int)ladder.size()-1;
    }

    // Feed a new bandwidth measurement (kbps).
    void update_bandwidth(double kbps) {
        if (kbps <= 0) return;
        if (ewma_bw < 0) ewma_bw = kbps;
        else ewma_bw = ALPHA*kbps + (1-ALPHA)*ewma_bw;

        // Select highest level that fits (with 10% headroom)
        int best = (int)ladder.size()-1; // default: lowest
        for (int i=0; i<(int)ladder.size(); i++) {
            if (ladder[i].required_kbps * 1.1 <= ewma_bw) { best=i; break; }
        }
        if (best != current_level) {
            fprintf(stderr,"[abr] BW %.0f kbps → quality %s\n",
                    ewma_bw, ladder[best].name.c_str());
            current_level = best;
        }
    }

    // Record a completed upload to infer bandwidth.
    void record_upload(int bytes, double elapsed_s) {
        if (elapsed_s <= 0) return;
        samples.push_back({bytes, elapsed_s});
        if ((int)samples.size() > MAX_SAMPLES) samples.pop_front();
        // Compute smoothed throughput from recent samples
        double total_b=0, total_t=0;
        for (auto& s : samples) { total_b+=s.bytes; total_t+=s.elapsed_s; }
        double kbps = (total_b*8/1000.0)/total_t;
        update_bandwidth(kbps);
    }

    const QualityLevel& current() const { return ladder[current_level]; }
    int currentIndex()            const { return current_level; }
    double currentBandwidth()     const { return ewma_bw; }
    const std::vector<QualityLevel>& allLevels() const { return ladder; }

    // Force a specific level by index.
    void force_level(int idx) {
        if (idx>=0 && idx<(int)ladder.size()) current_level=idx;
    }
};
