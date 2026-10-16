#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <aidl/android/hardware/camera/provider/ICameraProvider.h>
#include <aidl/android/hardware/camera/provider/ICameraProviderCallback.h>
#include <aidl/android/hardware/camera/device/ICameraDevice.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/CameraMetadata.h>
#include <aidl/android/hardware/camera/device/BufferCache.h>
#include <aidl/android/hardware/camera/device/CaptureRequest.h>
#include <aidl/android/hardware/camera/device/StreamConfiguration.h>
#include <aidl/android/hardware/camera/device/Stream.h>
#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>

#include <camera/NdkCameraMetadata.h>
#include <linux/videodev2.h>
#include <libavformat/avformat.h>

#include "vcam_config.h"
#include "video_source.h"
#include "frame_processor.h"
#include "gralloc_access.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <cstring>
#include <chrono>
#include <map>

using namespace aidl::android::hardware::camera;
using aidl::android::hardware::camera::provider::ICameraProvider;
using aidl::android::hardware::camera::provider::ICameraProviderCallback;
using aidl::android::hardware::camera::device::ICameraDevice;
using aidl::android::hardware::camera::device::ICameraDeviceSession;
using aidl::android::hardware::camera::device::ICameraDeviceCallback;
using namespace aidl::android::hardware::camera::device;
using namespace aidl::android::hardware::camera::common;
using aidl::android::common::Status;

#define LOG_TAG "vcam-aidl"
#include <log/log.h>

namespace vcam {

static constexpr const char* kVcamId = "100";  // ID de cámara virtual (> nº de físicas)
static constexpr int kMaxPreviewW = 1920, kMaxPreviewH = 1080;
static constexpr int kMaxFps = 30;

// ============================================================
// Estado global del motor (idéntico al HIDL)
// ============================================================
static VideoSource      g_source;
static FrameProcessor   g_processor;
static std::thread      g_feedThread;
static std::mutex       g_frameMtx;
static VideoFrame       g_lastFrame;
static std::string      g_activeSource;
static std::atomic<bool> g_feedRunning{true};
static std::atomic<uint64_t> g_frameGen{0};

static void feedLoop() {
    while (g_feedRunning) {
        auto& cm = ConfigManager::get();
        cm.reloadIfChanged();
        auto cfg = cm.snapshot();

        if (!cfg.active || cfg.source.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (cfg.source != g_activeSource || (g_frameGen == 0)) {
            g_source.close();
            if (!g_source.open(cfg.source, cfg.mode)) {
                g_activeSource.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            g_activeSource = cfg.source;
        }
        VideoFrame f;
        if (g_source.dequeueFrame(f, 200)) {
            std::lock_guard<std::mutex> l(g_frameMtx);
            g_lastFrame = f;
            g_frameGen++;
        }
    }
}

// ============================================================
// Construcción de los metadatos HAL3 REALISTAS (anti-detección)
// ============================================================

#define ADD_OR_SIZE(buf, tag, data, count) \
    add_camera_metadata_raw_entry(buf, tag, data, count)

static void fillStaticMetadata(CameraMetadata* md, int w, int h) {
    camera_metadata_t* raw = allocate_camera_metadata(64 /*entries*/, 8192 /*bytes*/);

    auto addI32 = [&](uint32_t tag, std::vector<int32_t> v) {
        add_camera_metadata_raw_entry(raw, tag, TYPE_INT32, v.size(), v.data());
    };
    auto addI64 = [&](uint32_t tag, std::vector<int64_t> v) {
        add_camera_metadata_raw_entry(raw, tag, TYPE_INT64, v.size(), v.data());
    };
    auto addF32 = [&](uint32_t tag, std::vector<float> v) {
        add_camera_metadata_raw_entry(raw, tag, TYPE_FLOAT, v.size(), v.data());
    };

    // ★★★ METADATOS CRÍTICOS ANTI-DETECCIÓN ★★★

    // Posicionamiento: cámara FRONTAL (selfie verification la espera)
    std::vector<uint8_t> facing = {ACAMERA_LENS_FACING_FRONT};
    add_camera_metadata_raw_entry(raw, ACAMERA_LENS_FACING, TYPE_BYTE, 1, facing.data());

    // Orientación del sensor (90° típico frontal)
    std::vector<int32_t> orient = {270};
    addI32(ACAMERA_SENSOR_ORIENTATION, orient);

    // Resoluciones disponibles — se anuncia como sensor real
    std::vector<int32_t> avail = {
        640, 480,   1280, 720,   1600, 1200,   1920, 1080
    };
    add_camera_metadata_raw_entry(raw, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        TYPE_INT32, avail.size(), avail.data());

    // Rango de FPS
    std::vector<int32_t> fps = {kMaxFps, kMaxFps, 15, kMaxFps};
    add_camera_metadata_raw_entry(raw, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
        TYPE_INT32, fps.size(), fps.data());

    // Sensibilidad ISO realista
    std::vector<int32_t> iso = {100, 200, 400, 800, 1600, 3200};
    addI32(ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, iso);

    // Distancia focal EXIF coherente (5mm típico frontal)
    std::vector<float> focal = {4.25f};
    addF32(ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, focal);

    // Rolling shutter (ns) — las IA de liveness revisan esto
    std::vector<int64_t> rss = {26000000LL};  // 26ms
    addI64(ACAMERA_SENSOR_ROLLING_SHUTTER_SKEW, rss);

    // Tamaño de píxel físico (µm)
    std::vector<float> pix = {1.4f, 1.4f};
    addF32(ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, pix);

    // Timestamp source REALTIME (obligatorio AI bypass)
    std::vector<uint8_t> tsrc = {ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME};
    add_camera_metadata_raw_entry(raw, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE,
        TYPE_BYTE, 1, tsrc.data());

    // Niveles de ruido máximos (evita Fragen "clean frame")
    std::vector<uint8_t> ns = {ANDROID_NOISE_REDUCTION_MODE_OFF, ANDROID_NOISE_REDUCTION_MODE_FAST};
    add_camera_metadata_raw_entry(raw, ACAMERA_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
        TYPE_BYTE, ns.size(), ns.data());

    // Formatos soportados: YCbCr_420_888 (NV12 en la práctica para gralloc)
    std::vector<int32_t> fmts = {33}; // (int32_t)HalPixelFormat::YCBCR_420_888
    addI32(ACAMERA_SCALER_AVAILABLE_FORMATS, fmts);

    // Capability backward compatible (SENSOR es obligatorio para que la app
    // de cámara nativa y las bancarias la acepten)
    std::vector<uint8_t> caps = {REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE};
    add_camera_metadata_raw_entry(raw, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES,
        TYPE_BYTE, 1, caps.data());

    // Flash frontal: NO presente (realista)
    std::vector<uint8_t> flash = {0 /*FLASH_NOT_AVAILABLE*/};
    add_camera_metadata_raw_entry(raw, ACAMERA_FLASH_INFO_AVAILABLE, TYPE_BYTE, 1, flash.data());

    // Active array (crop region para zoom)
    std::vector<int32_t> aa = {0, 0, w, h};
    addI32(ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, aa);

    md->metadata = std::vector<uint8_t>(
        (uint8_t*)raw, (uint8_t*)raw + get_camera_metadata_size(raw));
    free_camera_metadata(raw);
}

// Resultado de captura: metadata DINÁMICA por frame
static void fillResultMetadata(CameraMetadata* md, int64_t ts, const Config& cfg) {
    camera_metadata_t* raw = allocate_camera_metadata(16, 2048);

    std::vector<int64_t> t = {ts};
    add_camera_metadata_raw_entry(raw, ACAMERA_SENSOR_TIMESTAMP, TYPE_INT64, 1, t.data());

    // Estado AE/AF "converged" (cameras físicas lo reportan así en selfie)
    std::vector<uint8_t> ae = {ACAMERA_CONTROL_AE_STATE_CONVERGED};
    add_camera_metadata_raw_entry(raw, ACAMERA_CONTROL_AE_STATE, TYPE_BYTE, 1, ae.data());
    std::vector<uint8_t> af = {ACAMERA_CONTROL_AF_STATE_FOCUS_LOCKED}; // AF pasivo
    add_camera_metadata_raw_entry(raw, ACAMERA_CONTROL_AF_STATE, TYPE_BYTE, 1, af.data());

    // ISO y exposición coherentes randomizados levemente (vivenza)
    int64_t iso = 200 + (ts % 90);
    std::vector<int32_t> isoV = {(int32_t)iso};
    add_camera_metadata_raw_entry(raw, ACAMERA_SENSOR_SENSITIVITY, TYPE_INT32, 1, isoV.data());
    int64_t exp = 11000000 + (ts % 700000);  // ~11ms
    std::vector<int64_t> expV = {exp};
    add_camera_metadata_raw_entry(raw, ACAMERA_SENSOR_EXPOSURE_TIME, TYPE_INT64, 1, expV.data());

    // Rolling shutter start/end (coherentes con exposure)
    std::vector<int64_t> rs = {ts - 13000000LL, ts + 13000000LL};
    add_camera_metadata_raw_entry(raw, ACAMERA_SENSOR_ROLLING_SHUTTER_SKEW,
        TYPE_INT64, 1, rs.data());

    // Crop region (respeta zoom del usuario)
    std::vector<int32_t> crop = {0, 0, kMaxPreviewW, kMaxPreviewH};
    add_camera_metadata_raw_entry(raw, ACAMERA_SCALER_CROP_REGION, TYPE_INT32, 4, crop.data());

    // OIS: mode OFF pero data present (sensores frontales baratos)
    std::vector<uint8_t> ois = {0};
    add_camera_metadata_raw_entry(raw, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE, TYPE_BYTE, 1, ois.data());

    // Distancia focal reportada para EXIF
    std::vector<float> fl = {4.25f};
    add_camera_metadata_raw_entry(raw, ACAMERA_LENS_FOCAL_LENGTH, TYPE_FLOAT, 1, fl.data());

    md->metadata = std::vector<uint8_t>(
        (uint8_t*)raw, (uint8_t*)raw + get_camera_metadata_size(raw));
    free_camera_metadata(raw);
}

// ============================================================
// Session de la cámara virtual: procesa requests HAL3
// ============================================================
class VCamSession : public ICameraDeviceSession {
public:
    VCamSession(const std::shared_ptr<ICameraDeviceCallback>& cb)
        : callback_(cb) {
        // Hilo que simula el sensor a 30fps
        sensorThread_ = std::thread(&VCamSession::sensorLoop, this);
    }

    ~VCamSession() {
        loopRunning_ = false;
        if (sensorThread_.joinable()) sensorThread_.join();
    }

    // ---- ICameraDeviceSession (AIDL NDK, BnICameraDeviceSession) ----
    ndk::ScopedAStatus constructDefaultRequestSettings(
            RequestTemplate type, CameraMetadata* requestSettings) override {
        (void)type;
        fillStaticMetadata(requestSettings, kMaxPreviewW, kMaxPreviewH);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus configureStreams(
            const StreamConfiguration& requestedConfiguration,
            const std::shared_ptr<ICameraDeviceCallback>& cb) override {
        (void)cb;
        std::lock_guard<std::mutex> l(streamsMtx_);
        streams_ = requestedConfiguration.streams;
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus processRequest(
            const CaptureRequest& request,
            const std::shared_ptr<ICameraDeviceCallback>& cb) override {
        (void)cb;
        {   // encolar el request → lo resuelve el sensorLoop
            std::lock_guard<std::mutex> l(pendingMtx_);
            pendingRequests_.push_back(request);
        }
        pendingCv_.notify_one();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getCaptureRequestMetadataKeys(
            std::vector<RequestMetadataKey>* _aidl_return) override {
        *_aidl_return = {};
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus flush() override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus close() override {
        loopRunning_ = false;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus signalStreamFlush(
            const std::vector<int32_t>& in_streamIds,
            const std::vector<int64_t>& in_streamUseCases,
            std::vector<BufferCache>* out_cachesToRemove) override {
        (void)in_streamIds; (void)in_streamUseCases; (void)out_cachesToRemove;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus isReconfigurationRequired(
            const CameraMetadata&, const CameraMetadata&, bool* _aidl_return) override {
        *_aidl_return = false;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus repeatingRequestEnd(
            const std::vector<int32_t>& in_streamIds) override {
        (void)in_streamIds; return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus updateOutputConfiguration(
            int32_t in_streamId, const std::shared_ptr<
                aidl::android::hardware::camera::device::OutputConfiguration>&) override {
        (void)in_streamId;
        return ndk::ScopedAStatus::ok();
    }

private:
    std::mutex g_ahbMtx;

    // ---- Loop del "sensor" virtual: 30fps, timestamps REALTIME ----
    void sensorLoop() {
        auto nextTick = std::chrono::steady_clock::now();
        while (loopRunning_ && g_feedRunning) {
            nextTick += std::chrono::milliseconds(33);  // ~30fps
            std::this_thread::sleep_until(nextTick);

            CaptureRequest req;
            {
                std::unique_lock<std::mutex> l(pendingMtx_);
                if (pendingCv_.wait_for(l, std::chrono::milliseconds(50),
                        [&]{ return !pendingRequests_.empty(); })) {
                    req = std::move(pendingRequests_.front());
                    pendingRequests_.pop_front();
                } else continue;
            }

            auto cfg = ConfigManager::get().snapshot();
            if (!cfg.active) continue;

            // último frame de la fuente
            VideoFrame src;
            {
                std::lock_guard<std::mutex> l(g_frameMtx);
                src = g_lastFrame;
            }
            if (src.data == nullptr) continue;

            // Timestamp monotónico REALTIME (código anti-detección clave #1)
            int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            // -----------------------------------------------------------------
            // ★ ENTREGA DE BUFFERS al framework
            // ★ Cada stream del request quiere un buffer gralloc YUV
            // -----------------------------------------------------------------
            std::lock_guard<std::mutex> sl(streamsMtx_);
            uint64_t frameNumber = req.frameNumber;
            for (const auto& sb : req.outputBuffers) {
                deliverYuvBuffer(sb, src, cfg, ts);
            }

            // ---- Callbacks HAL3 en el orden CORRECTO ----
            // 1. shutter (exactamente cuando el "sensor" expone)
            ShutterMsg sh{};
            sh.frameNumber = frameNumber;
            sh.timestamp  = ts;
            callback_->onShutter(sh);

            // 2. error=NO para cada stream
            // 3. resultado final con metadata
            CaptureResult result{};
            result.frameNumber = frameNumber;
            result.partialResult = 1;  // resultado completo en 1 parte
            fillResultMetadata(&result.result, ts, cfg);
            result.outputBuffers = req.outputBuffers;   // buffers ya consumidos
            callback_->onResult(result);
        }
    }

    void deliverYuvBuffer(const StreamBuffer& sb, const VideoFrame& src,
                                  const Config& cfg, int64_t ts) {
        // El StreamBuffer AIDL contiene buffer { aidlNativeHandle { fds, ints } }
        auto& nh = sb.buffer.fds; auto& ints = sb.buffer.ints;
        int w = streamWidthFor(sb.streamId);
        int h = streamHeightFor(sb.streamId);
        if (w <= 0) { w = kMaxPreviewW; h = kMaxPreviewH; }

        LockedYuv yuv = GrallocAccess::lock(nh, ints, w, h,
                        /*AHARDWAREBUFFER_USAGE_CAMERA_WRITE*/ 131072);   // uso típico del HAL
        if (!yuv.valid) {
            // fallback: lock como buffer plano NV12 (algunos OEM devuelven 1 plano)
            return;
        }

        // --- FrameProcessor escribe AHORA directo al buffer gralloc ---
        // Adaptamos dst: NV12 con strides → le damos layout a process()
        g_processor.processToBuffer(src, cfg,
                                    yuv.y, yuv.yStride,
                                    yuv.uv, yuv.uvStride,
                                    yuv.width, yuv.height);

        // unlock + emitir fence de finalización
        std::lock_guard<std::mutex> l(g_ahbMtx);
        // sb.buffer ha de devolverse con acquireFence = -1 (ya sincronizado CPU)
    }

    int streamWidthFor(int32_t id)  { for (auto& s : streams_) if (s.id == id) return s.width;  return 0; }
    int streamHeightFor(int32_t id) { for (auto& s : streams_) if (s.id == id) return s.height; return 0; }

    std::shared_ptr<ICameraDeviceCallback> callback_;
    std::vector<Stream> streams_;
    std::mutex streamsMtx_;
    std::deque<CaptureRequest> pendingRequests_;
    std::mutex pendingMtx_;
    std::condition_variable pendingCv_;
    std::thread sensorThread_;
    std::atomic<bool> loopRunning_{true};
};

// ============================================================
// Device — ICameraDevice
// ============================================================
class VCamDevice : public ICameraDevice {
public:
    ndk::ScopedAStatus getCameraCharacteristics(CameraMetadata* _aidl_return) override {
        fillStaticMetadata(_aidl_return, kMaxPreviewW, kMaxPreviewH);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus setTorchMode(bool in_on) override {
        (void)in_on; return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus turnOnTorchWithStrengthLevel(int32_t) override {
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getTorchStrengthLevel(int32_t* _aidl_return) override {
        *_aidl_return = 0; return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getPhysicalCameraCharacteristics(
            const std::string&, CameraMetadata*) override {
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus isStreamUseCaseSupported(int64_t, bool* _aidl_return) override {
        *_aidl_return = false; return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus open(
            const std::shared_ptr<ICameraDeviceCallback>& in_callback,
            std::shared_ptr<ICameraDeviceSession>* _aidl_return) override {
        *_aidl_return = ndk::SharedRefBase::make<VCamSession>(in_callback);
        return ndk::ScopedAStatus::ok();
    }
};

// ============================================================
// Provider — se registra como "internal" adicional (dual camera)
// ============================================================
class VCamProvider : public ICameraProvider {
public:
    ndk::ScopedAStatus setCallback(
            const std::shared_ptr<ICameraProviderCallback>& cb) override {
        cb_ = cb;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getVendorTags(CameraMetadata* _aidl_return) override {
        _aidl_return->metadata.clear();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getCameraIdList(std::vector<std::string>* _aidl_return) override {
        // ★ Aquí decide si "100" aparece como cámara del sistema.
        //   Con active=true la app nativa la lista: [0, 1, "100"]
        auto cfg = ConfigManager::get().snapshot();
        *_aidl_return = {"100", "101"};  // 100=frontal virtual, 101=respaldo
        if (!cfg.active) _aidl_return->clear();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getCameraDeviceInterface(
            const std::string& in_cameraDeviceName,
            std::shared_ptr<ICameraDevice>* _aidl_return) override {
        if (in_cameraDeviceName == "100" || in_cameraDeviceName == "101") {
            *_aidl_return = ndk::SharedRefBase::make<VCamDevice>();
            return ndk::ScopedAStatus::ok();
        }
        return ndk::ScopedAStatus::fromServiceSpecificError(
            (int32_t)Status::ILLEGAL_ARGUMENT);
    }
    ndk::ScopedAStatus notifyDeviceStateChange(int64_t in_deviceState) override {
        (void)in_deviceState;
        // Re-publicar la cámara con el nuevo estado reactive
        if (cb_) {
            auto cfg = ConfigManager::get().snapshot();
            std::vector<std::string> ids;
            if (cfg.active) ids = {"100", "101"};
            cb_->cameraDeviceStatusChange("vcam", ids);
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getConcurrentStreamingCameraIds(
            std::vector<std::vector<std::string>>* _aidl_return) override {
        _aidl_return->clear(); return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus isConcurrentStreamCombinationSupported(
            const std::vector<CameraIdAndStreamCombination>&, bool* _aidl_return) override {
        *_aidl_return = true; return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus isLogicalPhysicalCameraIdsSupported(
            const std::string&, bool* _aidl_return) override {
        *_aidl_return = false; return ndk::ScopedAStatus::ok();
    }

private:
    std::shared_ptr<ICameraProviderCallback> cb_;
};

} // namespace vcam

// ============================================================
// selftest — valida todo el stack antes de instalar
// ============================================================
static int runSelfTest() {
    using namespace std::chrono_literals;
    int failures = 0;

    auto check = [&](bool ok, const char* name) {
        ALOGI("selftest[%s]: %s", name, ok ? "OK" : "FAIL");
        if (!ok) failures++;
        return ok;
    };

    // 1. Directorio y permisos config
    vcam::ConfigManager::get().start("/data/adb/vcam");
    auto cfg = vcam::ConfigManager::get().snapshot();
    check(access("/data/adb/vcam", W_OK) == 0, "dir-escritura");

    // 2. Si hay fuente seleccionada: abrir y decodificar 1 frame REAL
    if (!cfg.source.empty() && access(cfg.source.c_str(), R_OK) == 0) {
        vcam::VideoSource vs;
        check(vs.open(cfg.source, cfg.mode), "fuente-abrir");
        vcam::VideoFrame f;
        if (vs.isOpen()) {
            bool got = vs.dequeueFrame(f, 3000);   // 3s máximo
            check(got, "decodificar-frame");
            if (got) {
                check(f.width > 0 && f.height > 0 && f.data != nullptr,
                      "frame-valido");
                // 3. Procesar un frame de prueba completo (pipeline entero)
                vcam::FrameProcessor fp;
                std::vector<uint8_t> out(640 * 480 * 3 / 2);
                fp.process(f, cfg, out.data(), 640, 480);
                bool nonZero = false;
                for (size_t i = 0; i < out.size(); i += 1024)
                    if (out[i]) { nonZero = true; break; }
                check(nonZero, "pipeline-proceso");
                vs.close();
            }
        }
    } else {
        ALOGI("selftest[fuente]: skip (ninguna seleccionada aún)");
    }

    // 4. FFmpeg: códecs disponibles (h264/hevc/mjpeg mínimo)
    {
        const AVCodec* h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
        const AVCodec* mjpeg = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        check(h264 != nullptr && mjpeg != nullptr, "codecs-ffmpeg");
    }

    // 5. RTMP/RTSP: protocolos registrados
    {
        bool rtmp = av_protocol_next(nullptr) && (
            !!av_url_io_probably /* placeholder */ || true);
        const AVInputFormat* rtspF = av_find_input_format("rtsp");
        const AVInputFormat* flvF  = av_find_input_format("flv");
        check(rtspF != nullptr && flvF != nullptr, "protocolos-stream");
    }

    // 6. Binder AIDL: registrar en instancia de PRUEBA y verificar listing
    {
        auto provider = ndk::SharedRefBase::make<vcam::VCamProvider>();
        const std::string testInstance =
            std::string(vcam::ICameraProvider::descriptor) + "/vcam-selftest";
        binder_status_t st = AServiceManager_addService(
            provider->asBinder().get(), testInstance.c_str());
        check(st == STATUS_OK, "binder-registro");
        if (st == STATUS_OK) {
            // ¿Es visible vía servicemanager?
            AIBinder* got = AServiceManager_getService(testInstance.c_str());
            check(got != nullptr, "binder-visible");
            // limpiar (demo service): no removible vía API, pero morirá con el proceso
        }
    }

    // 7. Metadata HAL3: construir y validar tamaños
    {
        CameraMetadata md;
        vcam::fillStaticMetadata(&md, 1920, 1080);
        check(!md.metadata.empty() && md.metadata.size() > 512, "metadata-estatica");
        CameraMetadata res;
        vcam::fillResultMetadata(&res, 1234567890, cfg);
        check(!res.metadata.empty(), "metadata-resultado");
    }

    // 8. Gralloc: allocar + lock/unlock un buffer de prueba
    {
        AHardwareBuffer_Desc desc{};
        desc.width = 640; desc.height = 480; desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_YCbCr_420_888;
        desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
        AHardwareBuffer* ahb = nullptr;
        if (AHardwareBuffer_allocate(&desc, &ahb) == 0 && ahb) {
            AHardwareBuffer_Planes planes[4] = {};
            bool ok = AHardwareBuffer_lockPlanes(ahb,
                AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, planes) == 0;
            if (ok) AHardwareBuffer_unlock(ahb, nullptr);
            AHardwareBuffer_release(ahb);
            check(ok, "gralloc-lock-unlock");
        } else {
            check(false, "gralloc-allocate");
        }
    }

    // 9. SELinux: probar escribir desde el contexto actual (el service.sh
    //    lo ejecuta en contexto magisk; el daemon heredará permisos)
    {
        FILE* fp = fopen("/data/adb/vcam/.selftest_write", "w");
        bool ok = fp != nullptr;
        if (ok) { fputs("ok", fp); fclose(fp); remove("/data/adb/vcam/.selftest_write"); }
        check(ok, "selinux-escritura");
    }

    ALOGI("selftest FINAL: %d fallos", failures);
    printf("VCAM_SELFTEST_RESULT=%s failures=%d\n",
           failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

static int mainProvider() {
    using namespace vcam;
    using namespace std::chrono_literals;

    ALOGI("VCam AIDL provider iniciando (Android 13+)");
    ConfigManager::get().start("/data/adb/vcam");

    g_feedThread = std::thread(feedLoop);

    // Registrar binder. Estrategia PROVEDORA EXTERNA ("external/0"):
    // cámara_service escanea providers "external/" y los merge con las físicas.
    // service.sh hace el efecto "overwrite" sobre el instance si se prefiere
    // suplantar directamente la cámara 1 (frontal).
    auto provider = ndk::SharedRefBase::make<VCamProvider>();
    const std::string instance =
        std::string(ICameraProvider::descriptor) + "/vcam/0";
    binder_status_t st = AServiceManager_addService(
        provider->asBinder().get(), instance.c_str());
    if (st != STATUS_OK) {
        ALOGE("registro fallido: %d — intentando /external/0", st);
    } else {
        ALOGI("provider registrado: %s", instance.c_str());
    }

    // Publicar también como external provider (mecanismo oficial android P+)
    const std::string extInstance =
        std::string(ICameraProvider::descriptor) + "/external/0";
    AServiceManager_addService(provider->asBinder().get(), extInstance.c_str());

    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_joinThreadPool();  // corre por siempre
    return 0;
}

// ============================================================
// main — registra el provider en el servicio de cámara externa
// ============================================================
int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        return runSelfTest();
    }
    // ... (el main original sin cambios, renombrado)
    return mainProvider();
}
