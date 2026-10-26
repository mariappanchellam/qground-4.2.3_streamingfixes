// OpenH264 encoder + decoder wrappers.
// Uses only Cisco's libopenh264 — no FFmpeg, no GStreamer.
#pragma once
#include <wels/codec_api.h>
#include <wels/codec_app_def.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "scaler.h"

// ──────────────────────────────────────────────
// H.264 Decoder (one instance per incoming stream)
// ──────────────────────────────────────────────
class H264Decoder {
    ISVCDecoder* dec = nullptr;
    bool ok = false;
public:
    H264Decoder() {
        if (WelsCreateDecoder(&dec) != 0) { return; }
        SDecodingParam p{};
        p.sVideoProperty.size = sizeof(SVideoProperty);
        p.eEcActiveIdc = ERROR_CON_DISABLE;
        if (dec->Initialize(&p) != 0) {
            WelsDestroyDecoder(dec); dec=nullptr; return;
        }
        ok = true;
    }
    ~H264Decoder() {
        if (dec) { dec->Uninitialize(); WelsDestroyDecoder(dec); }
    }
    // Returns true if a complete frame is decoded.
    // yuv[0]=Y, [1]=U, [2]=V; strides[0]=Y stride, [1/2]=UV stride
    bool decode(const uint8_t* nal, int len,
                uint8_t* yuv[3], int strides[2], int& w, int& h) {
        if (!ok) return false;
        SBufferInfo info{}; void* data[3]{};
        DECODING_STATE r = dec->DecodeFrameNoDelay(nal,len,(uint8_t**)data,&info);
        if (info.iBufferStatus != 1) return false;
        (void)r;
        w=info.UsrData.sSystemBuffer.iWidth;
        h=info.UsrData.sSystemBuffer.iHeight;
        strides[0]=info.UsrData.sSystemBuffer.iStride[0];
        strides[1]=info.UsrData.sSystemBuffer.iStride[1];
        yuv[0]=(uint8_t*)data[0]; yuv[1]=(uint8_t*)data[1]; yuv[2]=(uint8_t*)data[2];
        return true;
    }
    bool ready() const { return ok; }
};

// ──────────────────────────────────────────────
// H.264 Encoder (one instance per output resolution)
// ──────────────────────────────────────────────
struct EncodeResult {
    // Collected NAL units from one frame, Annex-B (4-byte start codes).
    std::vector<uint8_t> data;
    bool is_keyframe = false;
};

class H264Encoder {
    ISVCEncoder* enc = nullptr;
    int width=0, height=0;
    bool ok = false;
    int  frame_num = 0;
public:
    // bitrate_kbps: target bitrate; fps: frame rate; keyframe_interval: seconds
    bool init(int w, int h, int bitrate_kbps, float fps=30.0f, int kf_interval_sec=2) {
        width=w; height=h;
        if (WelsCreateSVCEncoder(&enc) != 0) return false;

        SEncParamExt p{};
        enc->GetDefaultParams(&p);
        p.iUsageType          = CAMERA_VIDEO_REAL_TIME;
        p.fMaxFrameRate       = fps;
        p.iPicWidth           = w;
        p.iPicHeight          = h;
        p.iTargetBitrate      = bitrate_kbps * 1000;
        p.iMaxBitrate         = bitrate_kbps * 1500;
        p.iRCMode             = RC_BITRATE_MODE;
        p.uiIntraPeriod       = (int)(fps * kf_interval_sec);
        p.bEnableFrameSkip    = 0;
        p.iMultipleThreadIdc  = 2;
        p.bEnableDenoise      = 0;
        p.bEnableSceneChangeDetect = 1;
        p.bEnableBackgroundDetection = 1;
        p.bEnableAdaptiveQuant       = 1;
        p.sSpatialLayers[0].iVideoWidth  = w;
        p.sSpatialLayers[0].iVideoHeight = h;
        p.sSpatialLayers[0].fFrameRate   = fps;
        p.sSpatialLayers[0].iSpatialBitrate = bitrate_kbps * 1000;
        p.sSpatialLayers[0].uiProfileIdc = PRO_HIGH;
        p.sSpatialLayers[0].uiLevelIdc   = LEVEL_4_1;
        p.iSpatialLayerNum = 1;

        if (enc->InitializeExt(&p) != 0) {
            WelsDestroySVCEncoder(enc); enc=nullptr; return false;
        }
        int idc = videoFormatI420;
        enc->SetOption(ENCODER_OPTION_DATAFORMAT, &idc);
        ok = true;
        return true;
    }
    ~H264Encoder() {
        if (enc) { enc->Uninitialize(); WelsDestroySVCEncoder(enc); }
    }

    // Encode one YUV420 frame. Returns filled EncodeResult or empty on failure.
    EncodeResult encode(const YUVFrame& frame) {
        EncodeResult res;
        if (!ok || !enc) return res;
        SSourcePicture pic{};
        pic.iColorFormat  = videoFormatI420;
        pic.iPicWidth     = frame.w;
        pic.iPicHeight    = frame.h;
        pic.iStride[0]    = frame.strideY();
        pic.iStride[1]    = frame.strideUV();
        pic.iStride[2]    = frame.strideUV();
        pic.pData[0]      = const_cast<uint8_t*>(frame.Y());
        pic.pData[1]      = const_cast<uint8_t*>(frame.U());
        pic.pData[2]      = const_cast<uint8_t*>(frame.V());
        pic.uiTimeStamp   = frame_num++ * (1000 / 30);

        SFrameBSInfo info{};
        if (enc->EncodeFrame(&pic, &info) != videoFrameTypeSkip) {
            res.is_keyframe = (info.eFrameType == videoFrameTypeIDR);
            // Collect all NAL units from all layers
            for (int L=0; L<info.iLayerNum; L++) {
                const SLayerBSInfo& layer = info.sLayerInfo[L];
                for (int N=0; N<layer.iNalCount; N++) {
                    const uint8_t* p = layer.pBsBuf;
                    int offset = 0;
                    for(int k=0; k<N; k++) offset += layer.pNalLengthInByte[k];
                    res.data.insert(res.data.end(), p+offset,
                                    p+offset+layer.pNalLengthInByte[N]);
                }
            }
        }
        return res;
    }

    bool ready()  const { return ok; }
    int  getWidth()  const { return width; }
    int  getHeight() const { return height; }
};
