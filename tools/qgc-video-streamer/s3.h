// AWS S3 uploader using libcurl with AWS Signature V4 (CURLOPT_AWS_SIGV4).
// No AWS SDK required — just libcurl >= 7.75.
//
// Required environment variables:
//   QGC_AWS_ACCESS_KEY_ID
//   QGC_AWS_SECRET_ACCESS_KEY
//   QGC_AWS_REGION      (default: us-east-1)
//   QGC_S3_BUCKET
#pragma once
#include <curl/curl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>

struct S3Config {
    std::string access_key;
    std::string secret_key;
    std::string region      = "us-east-1";
    std::string bucket;
    std::string base_prefix = "";   // optional prefix inside bucket
    bool        public_read = false;

    static S3Config from_env() {
        S3Config c;
        auto ge=[](const char* k){ auto v=getenv(k); return v?std::string(v):""; };
        c.access_key = ge("QGC_AWS_ACCESS_KEY_ID");
        c.secret_key = ge("QGC_AWS_SECRET_ACCESS_KEY");
        c.region     = ge("QGC_AWS_REGION"); if(c.region.empty()) c.region="us-east-1";
        c.bucket     = ge("QGC_S3_BUCKET");
        c.base_prefix= ge("QGC_S3_PREFIX");
        return c;
    }
    bool valid() const { return !access_key.empty() && !secret_key.empty() && !bucket.empty(); }
};

// Read callback for libcurl (upload from memory buffer).
struct CurlReadCtx { const uint8_t* buf; size_t len; size_t offset; };
static size_t curl_read_cb(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* ctx = (CurlReadCtx*)ud;
    size_t remain = ctx->len - ctx->offset;
    size_t n = std::min(remain, sz*nm);
    memcpy(ptr, ctx->buf+ctx->offset, n);
    ctx->offset += n;
    return n;
}

// Sink for curl response body (discard).
static size_t curl_write_null(char*, size_t sz, size_t nm, void*) { return sz*nm; }

class S3Uploader {
    S3Config cfg;
    bool     initialised = false;
public:
    explicit S3Uploader(S3Config c) : cfg(std::move(c)) {
        if (!cfg.valid()) {
            fprintf(stderr,"[s3] Warning: missing AWS credentials/bucket — upload disabled\n");
            return;
        }
        curl_global_init(CURL_GLOBAL_ALL);
        initialised = true;
    }
    ~S3Uploader() { if(initialised) curl_global_cleanup(); }

    // Upload a local file to s3://bucket/key.
    // Returns HTTP status code (200/204 = success), or -1 on network error.
    int upload_file(const std::string& local_path, const std::string& key,
                    const std::string& content_type="video/MP2T") {
        if (!initialised) return -1;
        // Read file into memory
        std::ifstream f(local_path, std::ios::binary);
        if (!f) { fprintf(stderr,"[s3] Cannot open %s\n",local_path.c_str()); return -1; }
        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),{});
        return upload_mem(buf.data(),(int)buf.size(),key,content_type);
    }

    // Upload from memory buffer.
    int upload_mem(const uint8_t* data, int len, const std::string& key,
                   const std::string& content_type="video/MP2T") {
        if (!initialised) return -1;

        std::string full_key = cfg.base_prefix.empty() ? key : cfg.base_prefix+"/"+key;
        std::string url = "https://"+cfg.bucket+".s3."+cfg.region+".amazonaws.com/"+full_key;

        CURL* curl = curl_easy_init();
        if (!curl) return -1;

        // AWS SigV4 via libcurl — no manual HMAC needed
        std::string sigv4 = "aws:amz:"+cfg.region+":s3";
        std::string userpwd = cfg.access_key+":"+cfg.secret_key;

        CurlReadCtx ctx{data,(size_t)len,0};
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, ("Content-Type: "+content_type).c_str());
        hdrs = curl_slist_append(hdrs, ("Content-Length: "+std::to_string(len)).c_str());
        if (cfg.public_read)
            hdrs = curl_slist_append(hdrs, "x-amz-acl: public-read");

        curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
        curl_easy_setopt(curl, CURLOPT_UPLOAD,         1L);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,(curl_off_t)len);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION,   curl_read_cb);
        curl_easy_setopt(curl, CURLOPT_READDATA,       &ctx);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_null);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
        curl_easy_setopt(curl, CURLOPT_AWS_SIGV4,      sigv4.c_str());
        curl_easy_setopt(curl, CURLOPT_USERPWD,        userpwd.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            fprintf(stderr,"[s3] Upload %s failed: %s\n",
                    full_key.c_str(), curl_easy_strerror(res));
            return -1;
        }
        if (http_code >= 400) {
            fprintf(stderr,"[s3] Upload %s HTTP %ld\n", full_key.c_str(), http_code);
        }
        return (int)http_code;
    }

    // Upload a text/string (e.g. .m3u8 manifest).
    int upload_text(const std::string& text, const std::string& key,
                    const std::string& content_type="application/vnd.apple.mpegurl") {
        return upload_mem((const uint8_t*)text.c_str(),(int)text.size(),key,content_type);
    }

    bool ready() const { return initialised; }

    // Measure upload bandwidth by PUTting a small probe object.
    // Returns kbps, or -1 on failure.
    double measure_bandwidth_kbps() {
        if (!initialised) return -1;
        const int probe_sz = 256*1024; // 256 KB
        std::vector<uint8_t> probe(probe_sz, 0xAB);
        auto t0 = std::chrono::steady_clock::now();
        int r = upload_mem(probe.data(),probe_sz,"_bw_probe.bin","application/octet-stream");
        auto t1 = std::chrono::steady_clock::now();
        if (r < 200 || r >= 400) return -1;
        double elapsed = std::chrono::duration<double>(t1-t0).count();
        return (probe_sz*8/1000.0) / elapsed; // kbps
    }
};
