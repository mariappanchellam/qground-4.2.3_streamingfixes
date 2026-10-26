// Pure C++ bilinear YUV420 scaler — no external library needed.
#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstring>

struct YUVFrame {
    int w=0, h=0;
    std::vector<uint8_t> buf; // packed: Y plane then U then V

    uint8_t* Y()       { return buf.data(); }
    uint8_t* U()       { return buf.data() + w*h; }
    uint8_t* V()       { return buf.data() + w*h + (w/2)*(h/2); }
    const uint8_t* Y() const { return buf.data(); }
    const uint8_t* U() const { return buf.data() + w*h; }
    const uint8_t* V() const { return buf.data() + w*h + (w/2)*(h/2); }
    int strideY()  const { return w; }
    int strideUV() const { return w/2; }

    void resize(int nw, int nh) {
        w=nw; h=nh;
        buf.resize(nw*nh + 2*(nw/2)*(nh/2));
    }
};

// Bilinear downscale plane.
static inline void scale_plane(const uint8_t* src, int sw, int sh,
                                 uint8_t* dst, int dw, int dh) {
    for (int dy=0; dy<dh; dy++) {
        float sy = (dy+0.5f)*sh/(float)dh - 0.5f;
        int y0 = std::max(0,(int)sy), y1 = std::min(sh-1,y0+1);
        float fy = sy-y0; if(fy<0) fy=0;
        for (int dx=0; dx<dw; dx++) {
            float sx = (dx+0.5f)*sw/(float)dw - 0.5f;
            int x0 = std::max(0,(int)sx), x1 = std::min(sw-1,x0+1);
            float fx = sx-x0; if(fx<0) fx=0;
            float v = src[y0*sw+x0]*(1-fx)*(1-fy)
                    + src[y0*sw+x1]*fx*(1-fy)
                    + src[y1*sw+x0]*(1-fx)*fy
                    + src[y1*sw+x1]*fx*fy;
            dst[dy*dw+dx] = (uint8_t)(v+0.5f);
        }
    }
}

// Scale an I420 frame (YUV420 planar) to target dimensions.
// srcY/U/V: source planes; sW/sH: source size
// dst: output YUVFrame (will be resized)
static inline void scale_yuv420(const uint8_t* srcY, int srcStrY,
                                  const uint8_t* srcU, int srcStrUV,
                                  const uint8_t* srcV,
                                  int sW, int sH,
                                  YUVFrame& dst, int dW, int dH) {
    // Make dimensions even (required for YUV420)
    dW &= ~1; dH &= ~1;
    dst.resize(dW, dH);

    // Scale Y plane
    if (srcStrY == sW) {
        scale_plane(srcY, sW, sH, dst.Y(), dW, dH);
    } else {
        // Copy with stride to contiguous buffer first
        std::vector<uint8_t> tmp(sW*sH);
        for(int r=0; r<sH; r++) memcpy(tmp.data()+r*sW, srcY+r*srcStrY, sW);
        scale_plane(tmp.data(), sW, sH, dst.Y(), dW, dH);
    }

    // Scale U and V planes (half-resolution)
    int sW2=sW/2, sH2=sH/2, dW2=dW/2, dH2=dH/2;
    if (srcStrUV == sW2) {
        scale_plane(srcU, sW2, sH2, dst.U(), dW2, dH2);
        scale_plane(srcV, sW2, sH2, dst.V(), dW2, dH2);
    } else {
        std::vector<uint8_t> tu(sW2*sH2), tv(sW2*sH2);
        for(int r=0; r<sH2; r++){
            memcpy(tu.data()+r*sW2, srcU+r*srcStrUV, sW2);
            memcpy(tv.data()+r*sW2, srcV+r*srcStrUV, sW2);
        }
        scale_plane(tu.data(), sW2, sH2, dst.U(), dW2, dH2);
        scale_plane(tv.data(), sW2, sH2, dst.V(), dW2, dH2);
    }
}
