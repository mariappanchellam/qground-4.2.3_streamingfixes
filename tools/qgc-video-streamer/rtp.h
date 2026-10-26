// RTP receiver and H.264/RTP NAL assembler (RFC 6184)
// No external dependencies — pure POSIX sockets.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

struct __attribute__((packed)) RTPHdr {
    uint8_t  cc:4, x:1, p:1, v:2;
    uint8_t  pt:7, m:1;
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
};
static_assert(sizeof(RTPHdr) == 12, "RTP header size");

// Assembles RFC-6184 H.264/RTP payloads (single / STAP-A / FU-A) into
// complete Annex-B NAL units (4-byte start code prepended).
class NalAssembler {
    static const uint8_t SC[4];
    std::vector<uint8_t> fu_buf;
    bool    in_fu  = false;
    uint8_t fu_hdr = 0;
public:
    std::vector<std::vector<uint8_t>> feed(const uint8_t* p, int len, bool marker) {
        std::vector<std::vector<uint8_t>> out;
        if (len < 1) return out;
        uint8_t type = p[0] & 0x1F;

        if (type >= 1 && type <= 23) {
            auto& v = out.emplace_back();
            v.insert(v.end(), SC, SC+4);
            v.insert(v.end(), p, p+len);

        } else if (type == 24) {              // STAP-A
            for (int off = 1; off + 2 <= len; ) {
                uint16_t sz = (uint16_t(p[off])<<8)|p[off+1]; off += 2;
                if (off+sz > len) break;
                auto& v = out.emplace_back();
                v.insert(v.end(), SC, SC+4);
                v.insert(v.end(), p+off, p+off+sz);
                off += sz;
            }

        } else if (type == 28) {              // FU-A
            if (len < 2) return out;
            uint8_t fh = p[1];
            bool start = (fh>>7)&1, end = (fh>>6)&1;
            if (start) {
                in_fu  = true;
                fu_hdr = (p[0]&0xE0)|(fh&0x1F);
                fu_buf.clear();
                fu_buf.insert(fu_buf.end(), SC, SC+4);
                fu_buf.push_back(fu_hdr);
            }
            if (in_fu && len > 2)
                fu_buf.insert(fu_buf.end(), p+2, p+len);
            if (end && in_fu) {
                out.push_back(std::move(fu_buf));
                fu_buf.clear(); in_fu = false;
            }
        }
        (void)marker;
        return out;
    }
    void reset() { fu_buf.clear(); in_fu = false; }
};
inline const uint8_t NalAssembler::SC[4] = {0,0,0,1};

// Non-blocking UDP receive socket.
class UDPSock {
    int fd = -1;
public:
    bool open(uint16_t port) {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        int y=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&y,sizeof y);
        int buf=4<<20; setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&buf,sizeof buf);
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=INADDR_ANY;
        if (::bind(fd,(sockaddr*)&a,sizeof a)<0){ ::close(fd); fd=-1; return false; }
        return true;
    }
    // Returns bytes, 0=timeout, -1=error.
    int recv(uint8_t* buf, int cap, int ms=20) {
        fd_set r; FD_ZERO(&r); FD_SET(fd,&r);
        struct timeval tv{ms/1000,(ms%1000)*1000};
        int s=::select(fd+1,&r,nullptr,nullptr,&tv);
        if (s<=0) return s;
        return (int)::recv(fd,buf,cap,0);
    }
    bool valid() const { return fd>=0; }
    ~UDPSock(){ if(fd>=0) ::close(fd); }
};
