// Minimal MPEG-TS muxer for a single H.264 video track.
// Produces HLS-compatible .ts segments.
// No external dependency.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <cassert>

// ──────────────────────────────────────────────
// MPEG-TS constants
// ──────────────────────────────────────────────
static const int TS_PKT = 188;
static const uint16_t PID_PAT  = 0x0000;
static const uint16_t PID_PMT  = 0x0100;
static const uint16_t PID_VID  = 0x0101;
static const uint16_t PROG_NUM = 0x0001;

// CRC-32/MPEG-2 (poly 0x04C11DB7, MSB-first, no inversion)
static inline uint32_t crc32_mpeg(const uint8_t* d, size_t n) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i=0; i<n; i++) {
        crc ^= (uint32_t)d[i] << 24;
        for (int b=0; b<8; b++)
            crc = (crc<<1) ^ ((crc&0x80000000)?0x04C11DB7:0);
    }
    return crc;
}

static inline void put32be(uint8_t* p, uint32_t v) {
    p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}

// ──────────────────────────────────────────────
// Build one 188-byte TS packet
// ──────────────────────────────────────────────
static inline std::vector<uint8_t>
ts_packet(uint16_t pid, bool pusi, uint8_t& cc,
          const uint8_t* payload, int plen) {
    std::vector<uint8_t> pkt(TS_PKT, 0xFF);
    pkt[0] = 0x47;
    pkt[1] = (pusi?0x40:0x00) | (uint8_t)(pid>>8);
    pkt[2] = (uint8_t)pid;
    // adaptation: 0b00=payload only; we may need padding
    int fill = TS_PKT - 4 - plen;
    if (fill == 0) {
        pkt[3] = 0x10 | (cc&0x0F);           // payload only
        memcpy(pkt.data()+4, payload, plen);
    } else if (fill >= 2) {
        // adaptation field with stuffing
        pkt[3] = 0x30 | (cc&0x0F);           // adaptation + payload
        pkt[4] = (uint8_t)(fill-1);           // adaptation length
        pkt[5] = 0x00;                        // adaptation flags
        for (int i=6; i<4+fill; i++) pkt[i] = 0xFF;
        memcpy(pkt.data()+4+fill, payload, plen);
    } else {
        // fill==1: can't fit adaptation+flags; move 1 byte of payload to adaptation
        pkt[3] = 0x30 | (cc&0x0F);
        pkt[4] = 0x00;  // adaptation length = 0 (no flags byte needed? per spec: 0 = 1 byte)
        // Actually adaptation_field_length=0 means 0 bytes after the length field.
        // That pads 1 byte. Total overhead = 5 bytes; payload = 183.
        // But plen = 183 already, so this case shouldn't arise for reasonable payloads.
        pkt[4] = 0x00;
        memcpy(pkt.data()+5, payload, plen);
    }
    cc = (cc+1)&0x0F;
    return pkt;
}

// ──────────────────────────────────────────────
// TSMuxer — call write_pat_pmt() first, then write_nal() for each frame
// ──────────────────────────────────────────────
class TSMuxer {
    uint8_t cc_pat=0, cc_pmt=0, cc_vid=0;
    uint64_t dts_90k = 0; // 90 kHz clock
    float    fps     = 30.0f;
    int      frame_dur = 0; // ticks = 90000/fps

    void append(std::vector<uint8_t>& out, const uint8_t* pkt) {
        out.insert(out.end(), pkt, pkt+TS_PKT);
    }
public:
    explicit TSMuxer(float fps_=30.0f) : fps(fps_) {
        frame_dur = (int)(90000.0f/fps_+0.5f);
    }

    // Returns PAT + PMT packets (call at segment start, and ideally each IDR).
    std::vector<uint8_t> make_pat_pmt() {
        std::vector<uint8_t> out;
        out.reserve(2*TS_PKT);

        // ── PAT ──
        {
            uint8_t sec[13];
            sec[0]=0x00;                  // table_id = PAT
            sec[1]=0xB0; sec[2]=0x0D;    // section_syntax | section_length=13
            sec[3]=0x00; sec[4]=0x01;    // transport_stream_id=1
            sec[5]=0xC1;                  // version=0, current=1, section=0
            sec[6]=0x00; sec[7]=0x00;    // section 0, last 0
            sec[8]=0x00; sec[9]=0x01;    // program_number=1
            sec[10]=0xE1; sec[11]=(uint8_t)PID_PMT; // PMT PID
            uint32_t c=crc32_mpeg(sec,12);
            sec[12]=(uint8_t)(c>>24); // only need 1 byte? No — CRC is 4 bytes after section
            // Rebuild with 4-byte CRC:
            uint8_t section[17];
            section[0]=0x00;
            section[1]=0xB0; section[2]=0x0D;
            section[3]=0x00; section[4]=0x01;
            section[5]=0xC1;
            section[6]=0x00; section[7]=0x00;
            section[8]=0x00; section[9]=0x01;
            section[10]=0xE1; section[11]=(uint8_t)(PID_PMT>>8)|(0xE0);
            section[12]=(uint8_t)PID_PMT;
            uint32_t crc=crc32_mpeg(section,13);
            section[13]=crc>>24; section[14]=crc>>16; section[15]=crc>>8; section[16]=crc;
            uint8_t payload[184]={0};
            payload[0]=0x00; // pointer_field
            memcpy(payload+1,section,17);
            auto pkt=ts_packet(PID_PAT,true,cc_pat,payload,184);
            append(out,pkt.data());
        }
        // ── PMT ──
        {
            uint8_t section[20];
            section[0]=0x02;                   // table_id=PMT
            section[1]=0xB0; section[2]=0x12;  // section_length=18
            section[3]=0x00; section[4]=0x01;  // program_number=1
            section[5]=0xC1;
            section[6]=0x00; section[7]=0x00;
            // PCR PID
            section[8]= 0xE0|(uint8_t)(PID_VID>>8);
            section[9]=(uint8_t)PID_VID;
            section[10]=0xF0; section[11]=0x00; // program_info_length=0
            // Stream: H.264 (0x1B) on PID_VID
            section[12]=0x1B;
            section[13]=0xE0|(uint8_t)(PID_VID>>8);
            section[14]=(uint8_t)PID_VID;
            section[15]=0xF0; section[16]=0x00; // ES_info_length=0
            uint32_t crc=crc32_mpeg(section,17);
            section[17]=crc>>24; section[18]=crc>>16; section[19]=crc>>8;
            uint8_t ext[1]={(uint8_t)crc}; (void)ext;
            // Full section with CRC
            uint8_t sec2[21]; memcpy(sec2,section,20); sec2[20]=(uint8_t)crc;
            uint8_t payload[184]={0};
            payload[0]=0x00;
            memcpy(payload+1,sec2,21);
            auto pkt=ts_packet(PID_PMT,true,cc_pmt,payload,184);
            append(out,pkt.data());
        }
        return out;
    }

    // Wrap encoded H.264 Annex-B data into PES packets → TS packets.
    std::vector<uint8_t> write_nal(const uint8_t* data, int len, bool is_keyframe) {
        std::vector<uint8_t> out;

        // Build PES header with PTS (and PCR in adaptation field for keyframes)
        uint64_t pts = dts_90k;
        dts_90k += frame_dur;

        uint8_t pts_bytes[5];
        pts_bytes[0] = 0x21 | (uint8_t)((pts>>29)&0x0E);
        pts_bytes[1] = (uint8_t)(pts>>22);
        pts_bytes[2] = 0x01 | (uint8_t)((pts>>14)&0xFE);
        pts_bytes[3] = (uint8_t)(pts>>7);
        pts_bytes[4] = 0x01 | (uint8_t)((pts<<1)&0xFE);

        // PES header: 3(start) +1(stream_id) +2(len) +1(flags1) +1(flags2) +1(hdr_len) +5(pts) = 14
        uint8_t pes[14];
        pes[0]=0x00; pes[1]=0x00; pes[2]=0x01; pes[3]=0xE0;
        pes[4]=0x00; pes[5]=0x00;   // PES length 0 = unbounded
        pes[6]=0x80; pes[7]=0x80; pes[8]=0x05;  // flags: PTS present
        memcpy(pes+9, pts_bytes, 5);

        // First TS packet: adaptation (for PCR on IDR) + PES header + start of payload
        int pes_sz  = 14;
        int avail   = TS_PKT - 4; // without adaptation
        int pcr_sz  = is_keyframe ? 8 : 0; // adaptation with PCR = 8 bytes overhead
        if (is_keyframe) avail -= pcr_sz;

        // Assemble first payload chunk
        std::vector<uint8_t> first_payload;
        first_payload.insert(first_payload.end(), pes, pes+pes_sz);
        first_payload.insert(first_payload.end(), data, data+len);

        int fp_len = (int)first_payload.size();
        int offset = 0;
        bool first = true;

        while (offset < fp_len) {
            int chunk = std::min(fp_len-offset, TS_PKT-4);
            if (first && is_keyframe) chunk = std::min(chunk, TS_PKT-4-pcr_sz);

            std::vector<uint8_t> pkt(TS_PKT, 0xFF);
            pkt[0]=0x47;
            pkt[1]=(first?0x40:0x00)|(uint8_t)(PID_VID>>8);
            pkt[2]=(uint8_t)PID_VID;

            int hdr_off = 4;
            if (first && is_keyframe) {
                pkt[3]=0x30|(cc_vid&0x0F);
                // adaptation field with PCR
                pkt[4]=0x07; // adaptation_field_length=7
                pkt[5]=0x10; // PCR_flag set
                uint64_t pcr=pts;
                pkt[6]=(uint8_t)(pcr>>25);
                pkt[7]=(uint8_t)(pcr>>17);
                pkt[8]=(uint8_t)(pcr>>9);
                pkt[9]=(uint8_t)(pcr>>1);
                pkt[10]=(uint8_t)((pcr&1)<<7)|0x7E;
                pkt[11]=0x00;
                hdr_off=12;
            } else {
                int fill=TS_PKT-4-chunk;
                if (fill==0) {
                    pkt[3]=0x10|(cc_vid&0x0F);
                } else {
                    pkt[3]=0x30|(cc_vid&0x0F);
                    pkt[4]=(uint8_t)(fill-1);
                    pkt[5]=0x00;
                    for(int i=6;i<4+fill;i++) pkt[i]=0xFF;
                    hdr_off=4+fill;
                    chunk=TS_PKT-hdr_off;
                    if(chunk<0) chunk=0;
                }
            }
            int actual_chunk=std::min(chunk, fp_len-offset);
            if(actual_chunk>0)
                memcpy(pkt.data()+hdr_off, first_payload.data()+offset, actual_chunk);
            offset+=actual_chunk;
            cc_vid=(cc_vid+1)&0x0F;
            out.insert(out.end(), pkt.begin(), pkt.end());
            first=false;
        }
        return out;
    }

    void reset_clock() { dts_90k=0; cc_pat=cc_pmt=cc_vid=0; }
};
