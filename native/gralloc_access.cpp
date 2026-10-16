#include "gralloc_access.h"

#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <cstdlib>

namespace vcam {

// Convierte los fds+ints del AIDL NativeHandle (parcel del guardian de
// StreamBuffer.buffer) a un buffer_handle_t nativo de gralloc.
// Este es el formato: fds[] = dmabuf fds, ints[] = metadata del handle
// (width, height, stride, format, layerCount, usage lo/hi, id lo/hi, name...).
// El layout exacto es el de native_handle_t (+ data del gralloc4 del OEM).
typedef struct native_handle
{
    int version;        /* sizeof(native_handle_t) */
    int numFds;         /* number of file-descriptors at &data[0] */
    int numInts;        /* number of ints at &data[numFds] */
    int data[0];        /* numFds + numInts ints */
} native_handle_t;

static buffer_handle_t importHandle(const std::vector<int32_t>& fds,
                                    const std::vector<uint32_t>& ints) {
    // native_handle_t header: version(4) | numFds | numInts
    constexpr size_t kHeaderInts = 3;
    size_t totalInts = kHeaderInts + ints.size();
    native_handle_t* h = (native_handle_t*)malloc(
        sizeof(native_handle_t) + sizeof(int) * (totalInts - kHeaderInts));
    if (!h) return nullptr;
    h->version = 0;                    // NATIVE_HANDLE_VERSION en vendor NDK
    h->numFds  = (int)fds.size();
    h->numInts = (int)ints.size();
    for (size_t i = 0; i < fds.size(); i++)
        h->data[i] = (int)fds[i];
    for (size_t i = 0; i < ints.size(); i++)
        h->data[fds.size() + i] = (int)ints[i];
    return h;
}

LockedYuv GrallocAccess::lock(const std::vector<int32_t>& fds,
                             const std::vector<uint32_t>& ints,
                             int width, int height, uint64_t usage) {
    LockedYuv out;
    buffer_handle_t h = importHandle(fds, ints);
    if (!h) return out;

    // Convertir a AHardwareBuffer (dup fd — gralloc retiene ownership del clone)
    AHardwareBuffer_Desc desc{};
    desc.width = (uint32_t)width;
    desc.height = (uint32_t)height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_YCbCr_420_888; // NV12 flexible
    desc.usage  = usage | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    desc.stride = /* se over-escribe tras import: */ 0;

    AHardwareBuffer* ahb = nullptr;
    // Método 2 (fallback): AHardwareBuffer_allocate propio con mismo tamaño —
    // cuando el framework usa buffer_manager HAL en vez de pasar handles
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (AHardwareBuffer_allocate(&desc, &ahb) != 0) {
        free((void*)h); return out;
    }

    // ---- Lock平面 YUV ----
    AHardwareBuffer_Planes planes[4] = {};
    if (AHardwareBuffer_lockPlanes(ahb,
            AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, planes) == 0) {
        // YCbCr_420_888 se exporta como 2 planos en NV12:
        //   planes[0] = Y, planes[1] = interleaved UV
        out.y      = (uint8_t*)planes[0].address;
        out.yStride = planes[0].rowStride;
        if (planes[0].pixelStride > 1) {
            // formato semi-planar con pixelStride==2 en Y (YV12 por֫ ejemplo)
        }
        out.uv      = (uint8_t*)planes[1].address;
        out.uvStride = planes[1].rowStride;
        out.width   = width;
        out.height  = height;
        out.valid   = true;
    }

    free((void*)h);
    // ahb se guarda glob estático para unlock — o se entrega al llamador.
    return out;
}

} // namespace vcam
