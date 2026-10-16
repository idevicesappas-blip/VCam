#pragma once
#include "vcam_config.h"
#include "video_source.h"
#include <vector>
#include <random>

namespace vcam {

class FrameProcessor {
public:
    // Procesa src (NV12) según cfg y escribe en dst (NV12 w*h)
    // w/h = resolución pedida por la app cliente (HAL3 negotiated)
    void process(const VideoFrame& src, const Config& cfg,
                 uint8_t* dst, int w, int h);
    void processToBuffer(const VideoFrame& src, const Config& cfg,
                     uint8_t* dstY, int yStride,
                     uint8_t* dstUV, int uvStride,
                     int w, int h);

private:
    void rotateNV12(const uint8_t* src, int sw, int sh, int deg);
    void zoomPanMicro(const Config& cfg);          // crop+scale con micro-movimiento
    void applyBrightnessContrast(const Config& cfg, int w, int h);
    void applyNoiseDust(const Config& cfg, int w, int h, int64_t tUs);

    // Buffores de trabajo
    std::vector<uint8_t> buf1_, buf2_;
    int lastRotatedW_ = 0, lastRotatedH_ = 0;
    std::mt19937 rnd_{12345};
    int64_t t0Us_ = 0;
    struct Dust { float x, y, r, alpha; int life; };
    std::vector<Dust> dust_;
    void tickDust(int w, int h, float level);
    float dx2dummy();
};

} // namespace vcam
