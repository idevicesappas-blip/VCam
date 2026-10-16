#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace vcam {

struct Config {
    // Fuente
    bool active = false;              // switch START injection
    std::string mode = "video";       // "video" | "image" | "stream"
    std::string source = "";          // ruta mp4 / jpg o url rtmp://rtsp://http://

    // Capa de movimiento / pan
    float panX = 0.0f;               // -1.0 .. 1.0 (izq/der)
    float panY = 0.0f;               // -1.0 .. 1.0 (arriba/abajo)
    float zoom = 1.0f;                // 1.0 .. 3.0
    int   rotation = 0;                // 0/90/180/270

    // Ajustes de imagen
    float brightness = 0.0f;         // -100 .. 100
    float contrast   = 1.0f;         //   0.5 .. 2.0

    // Simulación de realidad
    float noiseLevel = 1.0f;          // 0..10 ruido de sensor
    float dustLevel  = 1.0f;          // 0..10 micro polvo
    bool  autoFocusWobble = true;     // micro-breathing de enfoque

    // Bypass detección IA selfie (anti-spoofing)
    bool  aiFaceBypass    = true;
    float microMoveScale  = 0.004f;   // sub-pixel breathing
    float microMoveRotation = 0.15f;  // grados de balanceo natural
};

class ConfigManager {
public:
    static ConfigManager& get();
    void  start(const std::string& path);   // dir /data/adb/vcam/
    Config snapshot();                      // copia segura para el hilo de frames
    bool  reloadIfChanged();                // poll mtime cada 500ms
private:
    std::mutex mtx_;
    Config cfg_;
    std::string path_;
    time_t lastMtime_ = 0;
};

} // namespace vcam
