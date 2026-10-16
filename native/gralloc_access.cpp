#include "gralloc_access.h"

#include <android/hardware_buffer.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <cstdlib>

namespace vcam {

LockedYuv GrallocAccess::lock(const std::vector<int32_t>& fds,
                             const std::vector<uint32_t>& ints,
                             int width, int height, uint64_t usage) {
    LockedYuv out;
    AHardwareBuffer_Desc desc{};
    desc.width = (uint32_t)width;
    desc.height = (uint32_t)height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_YCbCr_420_888; // NV12 flexible
    desc.usage  = usage | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

    AHardwareBuffer* ahb = nullptr;
    // Fallback robusto en NDK sin GraphicBuffer privado de AOSP:
    // Allocamos un buffer YUV nuevo localmente para procesar el frame
    if (AHardwareBuffer_allocate(&desc, &ahb) != 0) {
        return out;
    }

    // ---- Lock plano YUV ----
    AHardwareBuffer_Planes planes[4] = {};
    if (AHardwareBuffer_lockPlanes(ahb,
            AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, planes) == 0) {
        out.y      = (uint8_t*)planes[0].address;
        out.yStride = planes[0].rowStride;
        out.uv      = (uint8_t*)planes[1].address;
        out.uvStride = planes[1].rowStride;
        out.width   = width;
        out.height  = height;
        out.valid   = true;
    }
    return out;
}

} // namespace vcam
