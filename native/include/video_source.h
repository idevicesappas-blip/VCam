#pragma once
#include "vcam_config.h"
#include <cstdint>
#include <chrono>

namespace vcam {

struct VideoFrame {
    uint8_t* data = nullptr;   // NV12 o NV21 (w*h*1.5 bytes)
    int width = 0, height = 0;
    int64_t ptsUs = 0;
};

class VideoSource {
public:
    bool open(const std::string& url, const std::string& mode);
    void close();
    // Bloqueante con timeout; para imagen devuelve siempre la misma (loop)
    bool dequeueFrame(VideoFrame& out, int timeoutMs = 200);
    bool isOpen() const { return opened_; }
private:
    bool openAVFormat(const std::string& url);
    void convertToYuv(struct AVFrame* avf);   // sws_scale → NV12 1080p
    bool opened_ = false;
    struct Impl; Impl* impl_ = nullptr;
};

} // namespace vcam
