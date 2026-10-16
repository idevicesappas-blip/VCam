#pragma once
#include <cstdint>
#include <vector>

namespace vcam {

// Convierte el handle nativo del AIDL (ndk::ScopedFileDescriptor de dmabuf /
// AidlNativeHandle) a AHardwareBuffer, mapea el plano YUV en memoria y
// devuelve puntero base + strides. unlock() libera y hace flush del caché.
struct LockedYuv {
    uint8_t* y  = nullptr;   uint8_t* uv = nullptr;
    int yStride = 0, uvStride = 0;
    int width = 0, height = 0;
    bool valid = false;
};

class GrallocAccess {
public:
    // Importa el buffer del stream HAL3 y lo bloquea para escritura CPU
    static LockedYuv lock(const std::vector<int32_t>& fds,
                          const std::vector<uint32_t>& ints,
                          int width, int height, uint64_t usage);
    static void unlock(void* ahb);   // pasa el AHardwareBuffer* que lock() también devuelve via out
    static LockedYuv lockWithBuffer(void* /*AHardwareBuffer**/ outAhb,
                                    int width, int height);
};

} // namespace vcam
