#include <hidl/HidlTransportSupport.h>
#include <hidl/HidlLazyUtils.h>
#include <android/hardware/camera/provider/2.7/ICameraProvider.h>
#include "vcam_config.h"
#include "frame_processor.h"
#include "video_source.h"

using namespace android::hardware::camera::provider::V2_7;

static vcam::VideoSource      g_source;
static vcam::FrameProcessor   g_processor;
static bool g_engineReady = false;
std::string g_activeSource;

// ---- Hilo de fu ente: mantiene el último frame disponible siempre ----
static std::thread g_feedThread;
static std::mutex g_frameMtx;
static vcam::VideoFrame g_lastFrame;
static void feedLoop() {
    while (true) {
        auto cfg = vcam::ConfigManager::get().snapshot();
        vcam::ConfigManager::get().reloadIfChanged();
        if (!cfg.active || cfg.source.empty()) { usleep(100 * 1000); continue; }
        if (cfg.source != g_activeSource) {
            g_source.close();
            if (!g_source.open(cfg.source, cfg.mode)) { usleep(500 * 1000); continue; }
            g_activeSource = cfg.source;
        }
        vcam::VideoFrame f;
        if (g_source.dequeueFrame(f)) {
            std::lock_guard<std::mutex> l(g_frameMtx);
            g_lastFrame = f;
        }
    }
}

// ★ CADA VEZ QUE EL HAL REAL ESCRIBE un buffer de cámara de vuelta a la app
//   (post-proceso del pipeline HAL3), este hook lo sustituye:
extern "C" int vcam_hook_output_buffer(uint8_t* yuv, int w, int h) {
    std::lock_guard<std::mutex> l(g_frameMtx);
    auto cfg = vcam::ConfigManager::get().snapshot();
    if (!cfg.active || g_lastFrame.data == nullptr) return 0; // cámara física
    g_processor.process(g_lastFrame, cfg, yuv, w, h);
    return 1;
}

__attribute__((constructor))
static void vcam_entry() {
    vcam::ConfigManager::get().start("/data/adb/vcam");
    g_feedThread = std::thread(feedLoop);
}
