#include "video_source.h"
#include <thread>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace vcam {

constexpr int OUT_W = 1920, OUT_H = 1080;

struct VideoSource::Impl {
    AVFormatContext* fmt = nullptr;
    const AVCodec* dec = nullptr;
    AVCodecContext* cctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* yuv = nullptr;
    SwsContext* sws = nullptr;
    int vstreamIdx = -1;
    bool isImage = false;
    std::string url;
    std::vector<uint8_t> staticFrame;  // para modo imagen
    bool staticFrameDecoded = false;
};

bool VideoSource::openAVFormat(const std::string& url) {
    impl_->url = url;
    avformat_network_init();
    AVDictionary* opts = nullptr;
    // Para streams de OBS: baja latencia
    av_dict_set(&opts, "probesize",  "500000", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    if (avformat_open_input(&impl_->fmt, url.c_str(), nullptr, &opts) < 0) return false;
    if (avformat_find_stream_info(impl_->fmt, nullptr) < 0) return false;

    impl_->vstreamIdx = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &impl_->dec, 0);
    if (impl_->vstreamIdx < 0) return false;

    impl_->cctx = avcodec_alloc_context3(impl_->dec);
    avcodec_parameters_to_context(impl_->cctx, impl_->fmt->streams[impl_->vstreamIdx]->codecpar);
    if (avcodec_open2(impl_->cctx, impl_->dec, nullptr) < 0) return false;

    impl_->pkt   = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    impl_->yuv   = av_frame_alloc();
    // Buffer NV12 destino fijo

    int yuvSize = OUT_W * OUT_H * 3 / 2;
    impl_->staticFrame.resize(yuvSize);
    impl_->yuv->format = AV_PIX_FMT_NV12;
    impl_->yuv->width  = OUT_W;
    impl_->yuv->height = OUT_H;

    // Para asegurar buffer: realloc manual vía av_image_fill_arrays
    uint8_t* dst[4]; int lines[4];
    av_image_fill_arrays(dst, lines, impl_->staticFrame.data(),
                        AV_PIX_FMT_NV12, OUT_W, OUT_H, 1);
    for (int i = 0; i < 4 && dst[i]; i++) { impl_->yuv->data[i] = dst[i]; impl_->yuv->linesize[i] = lines[i]; }

    impl_->sws = sws_getContext(
        impl_->cctx->width, impl_->cctx->height, impl_->cctx->pix_fmt,
        OUT_W, OUT_H, AV_PIX_FMT_NV12,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    return true;
}

bool VideoSource::open(const std::string& url, const std::string& mode) {
    close();
    impl_ = new Impl();
    bool ok = openAVFormat(url);
    if (!ok) { close(); return false; }
    impl_->isImage = (mode == "image");
    opened_ = true;
    return true;
}

void VideoSource::close() {
    if (!impl_) return;
    if (impl_->sws)   sws_freeContext(impl_->sws);
    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->yuv)   av_frame_free(&impl_->yuv);
    if (impl_->pkt)   av_packet_free(&impl_->pkt);
    if (impl_->cctx)  avcodec_free_context(&impl_->cctx);
    if (impl_->fmt)   avformat_close_input(&impl_->fmt);
    delete impl_; impl_ = nullptr; opened_ = false;
}

static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool VideoSource::dequeueFrame(VideoFrame& out, int timeoutMs) {
    if (!opened_) return false;

    // Modo IMAGEN: decodificar una vez, entregar siempre (con micro-variación la
    // añade FrameProcessor; aquí devolvemos el mismo buffer)
    if (impl_->isImage) {
        if (impl_->staticFrameDecoded) {
            out.data   = impl_->staticFrame.data();
            out.width  = OUT_W; out.height = OUT_H;
            out.ptsUs  = nowUs();
            return true;
        }
    }

    int64_t deadline = nowUs() + (int64_t)timeoutMs * 1000;
    while (nowUs() < deadline) {
        int r = av_read_frame(impl_->fmt, impl_->pkt);
        if (r < 0) {
            // EOF: si es archivo → rewind y loop (vídeo circular)
            if (strcmp(impl_->fmt->iformat->name, "image2") != 0 && !impl_->isImage) {
                avformat_seek_file(impl_->fmt, impl_->vstreamIdx,
                                  INT64_MIN, 0, INT64_MAX, 0);
                avcodec_flush_buffers(impl_->cctx);
                continue;
            }
            return false;
        }
        if (impl_->pkt->stream_index != impl_->vstreamIdx) {
            av_packet_unref(impl_->pkt); continue;
        }
        if (avcodec_send_packet(impl_->cctx, impl_->pkt) == 0) {
            while (avcodec_receive_frame(impl_->cctx, impl_->frame) == 0) {
                sws_scale(impl_->sws, impl_->frame->data, impl_->frame->linesize,
                          0, impl_->cctx->height, impl_->yuv->data, impl_->yuv->linesize);
                // Copiamos al buffer estático (seguro para reuso)
                memcpy(impl_->staticFrame.data(), impl_->yuv->data[0],
                       (size_t)OUT_W * OUT_H);
                memcpy(impl_->staticFrame.data() + (size_t)OUT_W * OUT_H,
                       impl_->yuv->data[1], (size_t)OUT_W * OUT_H / 2);
                impl_->staticFrameDecoded = true;
                out.data = impl_->staticFrame.data();
                out.width = OUT_W; out.height = OUT_H;
                out.ptsUs = impl_->frame->pts != AV_NOPTS_VALUE
                          ? av_rescale_q(impl_->frame->pts,
                                impl_->fmt->streams[impl_->vstreamIdx]->time_base, {1,1000000})
                          : nowUs();
                av_packet_unref(impl_->pkt);
                return true;
            }
        }
        av_packet_unref(impl_->pkt);
    }
    return false;
}

} // namespace vcam
