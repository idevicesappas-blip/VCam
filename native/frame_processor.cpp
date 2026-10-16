#include "frame_processor.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace vcam {

static int64_t steadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// NV12: plano Y (w*h) + plano UV intercalado (w*h/2)

void FrameProcessor::rotateNV12(const uint8_t* src, int sw, int sh, int deg) {
    // Buffer destino intermedio
    buf1_.assign(src, src + (size_t)sw * sh * 3 / 2);
    if (deg == 0) { lastRotatedW_ = sw; lastRotatedH_ = sh; return; }

    if (deg == 180) {
        int w = sw, h = sh, uvOff = w * h;
        buf2_.resize(buf1_.size());
        // Y
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                buf2_[y * w + x] = buf1_[(h - 1 - y) * w + (w - 1 - x)];
        // UV (pares)
        for (int y = 0; y < h / 2; y++)
            for (int x = 0; x < w; x += 2) {
                int srcIdx = (h / 2 - 1 - y) * w + (w - 2 - x);
                int dstIdx = y * w + x;
                buf2_[uvOff + dstIdx]     = buf1_[uvOff + srcIdx];
                buf2_[uvOff + dstIdx + 1] = buf1_[uvOff + srcIdx + 1];
            }
        buf1_.swap(buf2_);
        lastRotatedW_ = w; lastRotatedH_ = h;
        return;
    }
    if (deg == 90 || deg == 270) {
        int w = sw, h = sh, uvOff = w * h;
        int nw = h, nh = w, duvOff = nw * nh;
        buf2_.assign(buf1_.size(), 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int nx = (deg == 90) ? (h - 1 - y) : y;
                int ny = (deg == 90) ? x : (w - 1 - x);
                buf2_[ny * nw + nx] = buf1_[y * w + x];
            }
        for (int y = 0; y < h / 2; y++)
            for (int x = 0; x < w; x += 2) {
                // cada par UV cubre macrobloque 2x2
                int sIdx = y * w + x;
                int nx = (deg == 90) ? (h - 1 - (2 * y))   : (2 * y);
                int ny = (deg == 90) ? x                    : (w - 2 - x);
                if (deg == 270) ny = x;  // ajuste par UV
                int dIdx = (ny / 2) * nw + nx;
                buf2_[duvOff + dIdx]     = buf1_[uvOff + sIdx];
                buf2_[duvOff + dIdx + 1] = buf1_[uvOff + sIdx + 1];
            }
        lastRotatedW_ = nw; lastRotatedH_ = nh;
        buf1_.swap(buf2_);
        return;
    }
}

// Zoom + pan + micro-movimiento (bypass IA anti-spoofing)
void FrameProcessor::zoomPanMicro(const Config& cfg) {
    int sw = lastRotatedW_, sh = lastRotatedH_;
    if (sw <= 0) return;

    // ---- Micro movimiento humano: una persona real NUNCA está quieta.
    // Seno lento de escala + balanceo. Anti-spoofing (liveness IA) lo exige.
    float microScale = 1.0f, microShiftX = 0.0f, microShiftY = 0.0f;
    if (cfg.aiFaceBypass) {
        double t = steadyUs() / 1e6;
        microScale  = 1.0f + cfg.microMoveScale  * sinf((float)(t * 0.7)) * 0.5f;
        microShiftX = cfg.microMoveScale * sh * 0.15f * sinf((float)(t * 0.43));
        microShiftY = cfg.microMoveScale * sh * 0.10f * cosf((float)(t * 0.31));
    }
    float totalZoom = cfg.zoom * microScale;
    if (totalZoom < 1.0f) totalZoom = 1.0f;

    // Región de recorte (crop) según zoom + pan
    int cropW = std::max(16, (int)(sw / totalZoom));
    int cropH = std::max(16, (int)(sh / totalZoom));
    int cx = sw / 2 + (int)(cfg.panX * (sw - cropW) * 0.5f + microShiftX);
    int cy = sh / 2 - (int)(cfg.panY * (sh - cropH) * 0.5f - microShiftY);
    cx = std::max(0, std::min(sw - cropW, cx));
    cy = std::max(0, std::min(sh - cropH, cy));

    // Escala nearest a la fuente completa (out = mismo tamaño sw x sh)
    buf2_.assign(buf1_.size(), 0);
    size_t uvSize = (size_t)sw * sh / 2;
    float ratioX = (float)cropW / sw;
    float ratioY = (float)cropH / sh;

    for (int y = 0; y < sh; y++) {
        const uint8_t* srow = &buf1_[(cy + (int)(y * ratioY)) * sw];
        uint8_t* drow = &buf2_[y * sw];
        for (int x = 0; x < sw; x++)
            drow[x] = srow[cx + (int)(x * ratioX)];
    }
    // UV
    for (int y = 0; y < sh / 2; y++) {
        const uint8_t* srow = &buf1_[sw * sh + ((cy / 2 + (int)(y * ratioY)) * sw)];
        uint8_t* drow = &buf2_[uvSize + (size_t)y * sw];
        for (int x = 0; x < sw; x += 2) {
            int sx = cx + ((int)(x * ratioX)) & ~1;
            drow[x] = srow[sx]; drow[x + 1] = srow[sx + 1];
        }
    }
    buf1_.swap(buf2_);
}

// Brillo + contraste (SOLO plano Y —.AllArgsConstructor luma)
void FrameProcessor::applyBrightnessContrast(const Config& cfg, int w, int h) {
    float b = cfg.brightness;        // -100..100
    float c = cfg.contrast;         // 0.5..2.0
    // LUT 256 — un solo cálculo por frame
    uint8_t lut[256];
    for (int i = 0; i < 256; i++) {
        int v = (int)(((i - 128) * c + 128) + (b * 2.0f));  // contraste respecto a gris medio
        lut[i] = (uint8_t)std::max(0, std::min(255, v));
    }
    uint8_t* Y = buf1_.data();
    for (int i = 0; i < w * h; i++) Y[i] = lut[Y[i]];
}

// Ruido de sensor + micro polvo persistente
void FrameProcessor::tickDust(int w, int h, float level) {
    if (dust_.size() < 24) {
        std::uniform_real_distribution<float> dx(0.0f, 1.0f), dr(1.5f, 6.0f);
        for (int i = dust_.size(); i < 24; i++)
            dust_.push_back({dx(rnd_), dx(rnd_), dr(rnd_), 0.05f + level * 0.01f, (int)(dr(rnd_) * 400)});
    }
    for (auto& d : dust_) {
        d.life--;
        if (d.life <= 0) { // nueva mota en otro sitio (el polvo "aparece" al mover)
            std::uniform_real_distribution<float> dx2(0.0f, 1.0f);
            std::uniform_real_distribution<float> dr2(1.5f, 6.0f);
            d.x = dx2(rnd_); d.y = dx2(rnd_); d.life = (int)(dr2(rnd_) * 400);
        }
    }
}

float FrameProcessor::dx2dummy() { return 0; } // (ver nota abajo)

void FrameProcessor::applyNoiseDust(const Config& cfg, int w, int h, int64_t tUs) {
    float n = cfg.noiseLevel;
    if (n > 0.01f) {
        // Ruido gaussiano en Y — simula gain de sensor en poca luz
        uint8_t* Y = buf1_.data();
        std::normal_distribution<float> g(0.0f, 1.8f * n);
        for (int i = 0; i < w * h; i++) {
            int v = Y[i] + (int)g(rnd_);
            Y[i] = v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
        }
    }
    // Micro polvo — puntos persistentes semitransparentes (no en cada frame igual:
    // al estilo de un objetivo real con suciedad)
    if (cfg.dustLevel > 0.01f) {
        if (dust_.empty() || steadyUs() - tUs > 0) {
            std::uniform_real_distribution<float> ux(0.0f, 1.0f);
            if (dust_.empty())
                for (int i = 0; i < 20; i++)
                    dust_.push_back({ux(rnd_), ux(rnd_), 1.5f + i * 0.25f, 0.04f * cfg.dustLevel, 300 + i * 50});
        }
        std::uniform_real_distribution<float> ux(0.0f, 1.0f);
        uint8_t* Y = buf1_.data();
        int seedTick = (int)(tUs / 300000000); // motas se regeneran ~cada 5 min
        for (auto& d : dust_) {
            d.life -= 1;
            if (d.life <= 0) { d.x = ux(rnd_); d.y = ux(rnd_); d.life = 400 + (int)(ux(rnd_) * 400); }
            int cxp = (int)(d.x * w), cyp = (int)(d.y * h);
            int r = (int)d.r;
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    int px = cxp + dx, py = cyp + dy;
                    if (px < 0 || py < 0 || px >= w || py >= h) continue;
                    float dist = sqrtf(dx * dx + dy * dy);
                    if (dist > r) continue;
                    // oscurecer ligeramente el píxel (polvo = +oscuridad difusa)
                    int idx = py * w + px;
                    int v = (int)(Y[idx] * (1.0f - d.alpha * (1.0f - dist / r)));
                    Y[idx] = v < 0 ? 0 : v;
                }
        }
        // breathing de autofocus: leve variación global de "nitidez" simulada
        // (implementada como micro variación de brillo ±1 en Y cada ~2s)
        if (cfg.autoFocusWobble) {
            float wob = sinf((tUs / 1000000.0f) * 1.3f) * 1.2f;
            for (int i = 0; i < w * h; i += 7) {  // submuestreo barato
                int v = Y[i] + (int)wob;
                Y[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
            }
        }
    }
}

// ---------- PIPELINE ----------
void FrameProcessor::process(const VideoFrame& src, const Config& cfg,
                             uint8_t* dst, int w, int h) {
    if (t0Us_ == 0) t0Us_ = steadyUs();
    int64_t tUs = steadyUs() - t0Us_;
    lastRotatedW_ = src.width; lastRotatedH_ = src.height;

    rotateNV12(src.data, src.width, src.height, ((cfg.rotation % 360) + 360) % 360);
    zoomPanMicro(cfg);                      // zoom + pan + micro-movimiento IA
    applyBrightnessContrast(cfg, lastRotatedW_, lastRotatedH_);
    applyNoiseDust(cfg, lastRotatedW_, lastRotatedH_, tUs);

    if (dst == nullptr) return;

    // Copiar al buffer de salida del request HAL (puede ser otra resolución — la
    // HAL3 negotiated stream config; aquí escala nearest si difiere)
    int sw = lastRotatedW_, sh = lastRotatedH_;
    if (w == sw && h == sh) {
        memcpy(dst, buf1_.data(), (size_t)w * h * 3 / 2);
    } else {
        float rx = (float)sw / w, ry = (float)sh / h;
        for (int y = 0; y < h; y++) {
            const uint8_t* s = &buf1_[((int)(y * ry)) * sw];
            memcpy(&dst[(size_t)y * w], &s[(int)(0 * 0)], 0); // placeholder línea
            for (int x = 0; x < w; x++) dst[y * w + x] = s[(int)(x * rx)];
        }
        for (int y = 0; y < h / 2; y++) {
            const uint8_t* s = &buf1_[(size_t)sw * sh + ((int)(y * ry) * sw)];
            uint8_t* d = &dst[(size_t)w * h + (size_t)y * w];
            for (int x = 0; x < w; x += 2) { int sx = ((int)(x * rx)) & ~1; d[x] = s[sx]; d[x + 1] = s[sx + 1]; }
        }
    }
}

void FrameProcessor::processToBuffer(const VideoFrame& src, const Config& cfg,
                                     uint8_t* dstY, int yStride,
                                     uint8_t* dstUV, int uvStride,
                                     int w, int h) {
    // 1. Pipeline completo en buf1_ (rot, zoom, pan, fx, noise, dust) — idéntico
    process(src, cfg, /*dst temporal*/ nullptr, w, h);      // llena buf1_
    // 2. Escritura final respetando strides del gralloc
    const uint8_t* srcY  = buf1_.data();
    const uint8_t* srcUV = buf1_.data() + (size_t)lastRotatedW_ * lastRotatedH_;
    for (int y = 0; y < h; y++)
        memcpy(dstY + (size_t)y * yStride, srcY + (size_t)y * lastRotatedW_, w);
    for (int y = 0; y < h / 2; y++)
        memcpy(dstUV + (size_t)y * uvStride, srcUV + (size_t)y * lastRotatedW_, w);
}

} // namespace vcam
